#include <vector>
#include <iostream>
#include <utility>
#include <numeric>
#include <algorithm>
#include <cuda.h>
#include "wrap_helpers.hpp"
#include <boost/python/stl_iterator.hpp>
#include "numpy_init.hpp"




// #define TRACE_CUDA




#ifdef TRACE_CUDA
#define CALL_GUARDED(NAME, ARGLIST) \
  { \
    std::cerr << #NAME << std::endl; \
    CUresult cu_status_code = NAME ARGLIST; \
    if (cu_status_code != CUDA_SUCCESS) \
      throw std::runtime_error(#NAME " failed: "\
          +std::string(cuda_error_to_str(cu_status_code)));\
  }
#else
#define CALL_GUARDED(NAME, ARGLIST) \
  { \
    CUresult cu_status_code = NAME ARGLIST; \
    if (cu_status_code != CUDA_SUCCESS) \
      throw std::runtime_error(#NAME " failed: "\
          +std::string(cuda_error_to_str(cu_status_code)));\
  }
#endif




namespace
{
  const char *cuda_error_to_str(CUresult e)
  {
    switch (e)
    {
      case CUDA_SUCCESS: return "success";
      case CUDA_ERROR_INVALID_VALUE: return "invalid value";
      case CUDA_ERROR_OUT_OF_MEMORY: return "out of memory";
      case CUDA_ERROR_NOT_INITIALIZED: return "not initialized";
      case CUDA_ERROR_DEINITIALIZED: return "deinitialized";

      case CUDA_ERROR_NO_DEVICE: return "no device";
      case CUDA_ERROR_INVALID_DEVICE: return "invalid device";

      case CUDA_ERROR_INVALID_IMAGE: return "invalid image";
      case CUDA_ERROR_INVALID_CONTEXT: return "invalid context";
      case CUDA_ERROR_CONTEXT_ALREADY_CURRENT: return "context already current";
      case CUDA_ERROR_MAP_FAILED: return "map failed";
      case CUDA_ERROR_UNMAP_FAILED: return "unmap failed";
      case CUDA_ERROR_ARRAY_IS_MAPPED: return "array is mapped";
      case CUDA_ERROR_ALREADY_MAPPED: return "already mapped";
      case CUDA_ERROR_NO_BINARY_FOR_GPU: return "no binary for gpu";
      case CUDA_ERROR_ALREADY_ACQUIRED: return "already acquired";
      case CUDA_ERROR_NOT_MAPPED: return "not mapped";

      case CUDA_ERROR_INVALID_SOURCE: return "invalid source";
      case CUDA_ERROR_FILE_NOT_FOUND: return "file not found";

      case CUDA_ERROR_INVALID_HANDLE: return "invalid handle";

      case CUDA_ERROR_NOT_FOUND: return "not found";

      case CUDA_ERROR_NOT_READY: return "not ready";

      case CUDA_ERROR_LAUNCH_FAILED: return "launch failed";
      case CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES: return "launch out of resources";
      case CUDA_ERROR_LAUNCH_TIMEOUT: return "launch timeout";
      case CUDA_ERROR_LAUNCH_INCOMPATIBLE_TEXTURING: return "launch incompatible texturing";

      case CUDA_ERROR_UNKNOWN: return "unknown";

      default: return "invalid error code";
    }
  }




  // device -------------------------------------------------------------------
  class context;

  class device
  {
    private:
      CUdevice m_device;

    public:
      device(CUdevice dev)
        : m_device(dev)
      { }

      static int count()
      {
        int result;
        CALL_GUARDED(cuDeviceGetCount, (&result));
        return result;
      }

      std::string name()
      {
        char buffer[1024];
        CALL_GUARDED(cuDeviceGetName, (buffer, sizeof(buffer), m_device));
        return buffer;
      }

      py::tuple compute_capability()
      {
        int major, minor;
        CALL_GUARDED(cuDeviceComputeCapability, (&major, &minor, m_device));
        return py::make_tuple(major, minor);
      }

      unsigned int total_memory()
      {
        unsigned int bytes;
        CALL_GUARDED(cuDeviceTotalMem, (&bytes, m_device));
        return bytes;
      }

      int get_attribute(CUdevice_attribute attr)
      {
        int result;
        CALL_GUARDED(cuDeviceGetAttribute, (&result, attr, m_device));
        return result;
      }

      context *make_context(unsigned int flags);
  };

  void init(unsigned int flags) { CALL_GUARDED(cuInit, (flags)); }

  device *make_device(int ordinal)
  { 
    CUdevice result;
    CALL_GUARDED(cuDeviceGet, (&result, ordinal)); 
    return new device(result);
  }




  // context ------------------------------------------------------------------
  struct context
  {
    private:
      CUcontext m_context;
      bool m_valid;

    public:
      context(CUcontext ctx, bool borrowed)
        : m_context(ctx), m_valid(!borrowed)
      { 
        if (borrowed)
        {
          CALL_GUARDED(cuCtxAttach, (&m_context, 0));
          m_valid = true;
        }
      }

      context(context const &src)
        : m_context(src.m_context), m_valid(false)
      { 
        CALL_GUARDED(cuCtxAttach, (&m_context, 0));
        m_valid = true;
      }

      context &operator=(const context &src)
      {
        detach();
        m_context = src.m_context;
        CALL_GUARDED(cuCtxAttach, (&m_context, 0));
        m_valid = true;
      }

      ~context()
      { detach(); }

      void detach()
      {
        if (m_valid)
        {
          CALL_GUARDED(cuCtxDetach, (m_context));
          m_valid = false;
        }
      }

      void push()
      { CALL_GUARDED(cuCtxPushCurrent, (m_context)); }

      void pop()
      { 
        CUcontext popped;
        CALL_GUARDED(cuCtxPopCurrent, (&popped)); 
        if (popped != m_context)
          throw std::runtime_error("popped the wrong context");
      }

      static device get_device()
      { 
        CUdevice dev;
        CALL_GUARDED(cuCtxGetDevice, (&dev)); 
        return device(dev);
      }

      static void synchronize()
      { CALL_GUARDED(cuCtxSynchronize, ()); }
  };

  context *device::make_context(unsigned int flags)
  {
    CUcontext ctx;
    CALL_GUARDED(cuCtxCreate, (&ctx, flags, m_device));
    return new context(ctx, false);
  }




  // streams ------------------------------------------------------------------
  class stream : public boost::noncopyable
  {
    private:
      CUstream m_stream;

    public:
      stream(unsigned int flags=0)
      { CALL_GUARDED(cuStreamCreate, (&m_stream, flags)); }

      ~stream()
      { CALL_GUARDED(cuStreamDestroy, (m_stream)); }

      void synchronize()
      { CALL_GUARDED(cuStreamSynchronize, (m_stream)); }

      CUstream data() const
      { return m_stream; }

      bool is_done() const
      { 
#ifdef TRACE_CUDA
        std::cerr << "cuStreamQuery" << std::endl;
#endif
        CUresult result = cuStreamQuery(m_stream);
        switch (result)
        {
          case CUDA_SUCCESS: 
            return true;
          case CUDA_ERROR_NOT_READY: 
            return false;
          default:
            throw std::runtime_error("cuStreamQuery return unexpected error: "
                +std::string(cuda_error_to_str(result)));
        }
      }
  };




  // module -------------------------------------------------------------------
  class function;

  struct module : public boost::noncopyable
  {
    private:
      CUmodule m_module;

    public:
      module(CUmodule mod)
        : m_module(mod)
      { }

      ~module()
      {
        CALL_GUARDED(cuModuleUnload, (m_module));
      }

      function get_function(const char *name);
      py::tuple get_global(const char *name)
      {
        CUdeviceptr devptr;
        unsigned int bytes;
        CALL_GUARDED(cuModuleGetGlobal, (&devptr, &bytes, m_module, name));
        return py::make_tuple(devptr, bytes);
      }

      // get_texref(const char *name);
  };

  module *load_module(const char *filename)
  {
    CUmodule mod;
    CALL_GUARDED(cuModuleLoad, (&mod, filename));
    return new module(mod);
  }

  module *module_from_buffer(py::object buffer)
  {
    const char *buf;
    Py_ssize_t len;
    if (PyObject_AsCharBuffer(buffer.ptr(), &buf, &len))
      throw py::error_already_set();
    CUmodule mod;
    CALL_GUARDED(cuModuleLoadData, (&mod, buf));
    return new module(mod);
  }



  // function -----------------------------------------------------------------
  struct function
  {
    private:
      CUfunction m_function;

    public:
      function(CUfunction func)
        : m_function(func)
      { }

      void set_block_shape(int x, int y, int z)
      { CALL_GUARDED(cuFuncSetBlockShape, (m_function, x, y, z)); }
      void set_shared_size(unsigned int bytes)
      { CALL_GUARDED(cuFuncSetSharedSize, (m_function, bytes)); }

      void param_set_size(unsigned int bytes)
      { CALL_GUARDED(cuParamSetSize, (m_function, bytes)); }
      void param_set(int offset, unsigned int value)
      { CALL_GUARDED(cuParamSeti, (m_function, offset, value)); }
      void param_set(int offset, float value)
      { CALL_GUARDED(cuParamSeti, (m_function, offset, value)); }
      void param_setv(int offset, py::object buffer)
      { 
        const void *buf;
        Py_ssize_t len;
        if (PyObject_AsReadBuffer(buffer.ptr(), &buf, &len))
          throw py::error_already_set();
        CALL_GUARDED(cuParamSetv, (m_function, offset, const_cast<void *>(buf), len)); 
      }
      /* FIXME
      void param_set(int offset, const texture_ref &tr)
      {
      }
      */

      void launch()
      { CALL_GUARDED(cuLaunch, (m_function)); }
      void launch_grid(int grid_width, int grid_height)
      { CALL_GUARDED(cuLaunchGrid, (m_function, grid_width, grid_height)); }
      void launch_grid_async(int grid_width, int grid_height, const stream &s)
      { CALL_GUARDED(cuLaunchGridAsync, (m_function, grid_width, grid_height, s.data())); }
  };

  function module::get_function(const char *name)
  {
    CUfunction func;
    CALL_GUARDED(cuModuleGetFunction, (&func, m_module, name));
    return function(func);
  }




  // device memory ------------------------------------------------------------
  struct device_allocation : public boost::noncopyable
  {
    private:
      CUdeviceptr m_devptr;

    public:
      device_allocation(CUdeviceptr devptr)
        : m_devptr(devptr)
      { }

      ~device_allocation()
      {
        CALL_GUARDED(cuMemFree, (m_devptr));
      }
      
      operator CUdeviceptr()
      { return m_devptr; }
  };

  py::tuple mem_get_info()
  {
    unsigned int free, total;
    CALL_GUARDED(cuMemGetInfo, (&free, &total));
    return py::make_tuple(free, total);
  }

  device_allocation *mem_alloc(unsigned int bytes)
  {
    CUdeviceptr devptr;
    CALL_GUARDED(cuMemAlloc, (&devptr, bytes));
    return new device_allocation(devptr);
  }

  py::tuple mem_alloc_pitch(
      unsigned int width, unsigned int height, unsigned int access_size)
  {
    CUdeviceptr devptr;
    unsigned int pitch;
    CALL_GUARDED(cuMemAllocPitch, (&devptr, &pitch, width, height, access_size));
    return py::make_tuple(
        new device_allocation(devptr),
        pitch);
  }

  py::tuple mem_get_address_range(CUdeviceptr ptr)
  {
    CUdeviceptr base;
    unsigned int size;
    CALL_GUARDED(cuMemGetAddressRange, (&base, &size, ptr));
    return py::make_tuple(base, size);
  }

  void memcpy_htod(CUdeviceptr dst, py::object src)
  {
    const void *buf;
    Py_ssize_t len;
    if (PyObject_AsReadBuffer(src.ptr(), &buf, &len))
      throw py::error_already_set();
    CALL_GUARDED(cuMemcpyHtoD, (dst, buf, len));
  }

  void memcpy_dtoh(py::object dest, CUdeviceptr src)
  {
    void *buf;
    Py_ssize_t len;
    if (PyObject_AsWriteBuffer(dest.ptr(), &buf, &len))
      throw py::error_already_set();
    CALL_GUARDED(cuMemcpyDtoH, (buf, src, len));
  }

  void memcpy_htod_async(CUdeviceptr dst, py::object src, const stream &s)
  {
    const void *buf;
    Py_ssize_t len;
    if (PyObject_AsReadBuffer(src.ptr(), &buf, &len))
      throw py::error_already_set();
    CALL_GUARDED(cuMemcpyHtoDAsync, (dst, buf, len, s.data()));
  }

  void memcpy_dtoh_async(py::object dest, CUdeviceptr src, const stream &s)
  {
    void *buf;
    Py_ssize_t len;
    if (PyObject_AsWriteBuffer(dest.ptr(), &buf, &len))
      throw py::error_already_set();
    CALL_GUARDED(cuMemcpyDtoHAsync, (buf, src, len, s.data()));
  }



  // host memory --------------------------------------------------------------
  struct host_allocation : public boost::noncopyable
  {
    private:
      void *m_data;

    public:
      host_allocation(unsigned bytesize)
        : m_data(0)
      { CALL_GUARDED(cuMemAllocHost, (&m_data, bytesize)); }

      ~host_allocation()
      { CALL_GUARDED(cuMemFreeHost, (m_data)); }
      
      void *data()
      { return m_data; }
  };




  inline
  npy_intp size_from_dims(int ndim, const npy_intp *dims)
  {
    if (ndim != 0)
      return std::accumulate(dims, dims+ndim, 1, std::multiplies<npy_intp>());
    else
      return 1;
  }




  py::handle<> pagelocked_empty(py::object shape, py::object dtype, py::object order_py)
  {
    PyArray_Descr *tp_descr;
    if (PyArray_DescrConverter(dtype.ptr(), &tp_descr) != NPY_SUCCEED)
      throw py::error_already_set();

    std::vector<npy_intp> dims;
    std::copy(
        py::stl_input_iterator<npy_intp>(shape),
        py::stl_input_iterator<npy_intp>(),
        back_inserter(dims));

    std::auto_ptr<host_allocation> alloc(
        new host_allocation(
          tp_descr->elsize*size_from_dims(dims.size(), &dims.front())));

    NPY_ORDER order = PyArray_CORDER;
    PyArray_OrderConverter(order_py.ptr(), &order);

    int flags = 0;
    if (order == PyArray_FORTRANORDER)
      flags |= NPY_FARRAY;
    else if (order == PyArray_CORDER)
      flags |= NPY_CARRAY;
    else
      throw std::runtime_error("unrecognized order specifier");

    py::handle<> result = py::handle<>(PyArray_NewFromDescr(
        &PyArray_Type, tp_descr,
        dims.size(), &dims.front(), /*strides*/ NULL,
        alloc->data(), flags, /*obj*/NULL));

    py::object alloc_py(alloc.release());
    PyArray_BASE(result.get()) = alloc_py.ptr();
    Py_INCREF(alloc_py.ptr());

    return result;
  }




  // events -------------------------------------------------------------------
  class event : public boost::noncopyable
  {
    private:
      CUevent m_event;

    public:
      event(unsigned int flags=0)
      { CALL_GUARDED(cuEventCreate, (&m_event, flags)); }

      ~event()
      { CALL_GUARDED(cuEventDestroy, (m_event)); }

      void record()
      { CALL_GUARDED(cuEventRecord, (m_event, 0)); }

      void record_in_stream(stream const &str)
      { CALL_GUARDED(cuEventRecord, (m_event, str.data())); }

      void synchronize()
      { CALL_GUARDED(cuEventSynchronize, (m_event)); }

      bool query() const
      { 
#ifdef TRACE_CUDA
        std::cerr << "cuEventQuery" << std::endl;
#endif
        CUresult result = cuEventQuery(m_event);
        switch (result)
        {
          case CUDA_SUCCESS: 
            return true;
          case CUDA_ERROR_NOT_READY: 
            return false;
          default:
            throw std::runtime_error("cuEventQuery failed: "
                +std::string(cuda_error_to_str(result)));
        }
      }

      float time_since(event const &start)
      {
        float result;
        CALL_GUARDED(cuEventElapsedTime, (&result, start.m_event, m_event));
        return result;
      }

      float time_till(event const &end)
      {
        float result;
        CALL_GUARDED(cuEventElapsedTime, (&result, m_event, end.m_event));
        return result;
      }
  };
}




BOOST_PYTHON_MODULE(_driver)
{
  py::enum_<CUctx_flags>("ctx_flags")
    .value("SCHED_AUTO", CU_CTX_SCHED_AUTO)
    .value("SCHED_SPIN", CU_CTX_SCHED_SPIN)
    .value("SCHED_YIELD", CU_CTX_SCHED_YIELD)
    .value("SCHED_MASK", CU_CTX_SCHED_MASK)
    .value("SCHED_FLAGS_MASK", CU_CTX_FLAGS_MASK)
    ;
  py::enum_<CUarray_format>("array_format")
    .value("UNSIGNED_INT8", CU_AD_FORMAT_UNSIGNED_INT8)
    .value("UNSIGNED_INT16", CU_AD_FORMAT_UNSIGNED_INT16)
    .value("UNSIGNED_INT32", CU_AD_FORMAT_UNSIGNED_INT32)
    .value("SIGNED_INT8"   , CU_AD_FORMAT_SIGNED_INT8)
    .value("SIGNED_INT16"  , CU_AD_FORMAT_SIGNED_INT16)
    .value("SIGNED_INT32"  , CU_AD_FORMAT_SIGNED_INT32)
    .value("HALF"          , CU_AD_FORMAT_HALF)
    .value("FLOAT"         , CU_AD_FORMAT_FLOAT)
    ;

  py::enum_<CUaddress_mode>("address_mode")
    .value("WRAP", CU_TR_ADDRESS_MODE_WRAP)
    .value("CLAMP", CU_TR_ADDRESS_MODE_CLAMP)
    .value("MIRROR", CU_TR_ADDRESS_MODE_MIRROR)
    ;

  py::enum_<CUfilter_mode>("filter_mode")
    .value("POINT", CU_TR_FILTER_MODE_POINT)
    .value("LINEAR", CU_TR_FILTER_MODE_LINEAR)
    ;
  py::enum_<CUdevice_attribute>("device_attribute")
    .value("MAX_THREADS_PER_BLOCK", CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK)
    .value("MAX_BLOCK_DIM_X", CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X)
    .value("MAX_BLOCK_DIM_Y", CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y)
    .value("MAX_BLOCK_DIM_Z", CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z)
    .value("MAX_GRID_DIM_X", CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X)
    .value("MAX_GRID_DIM_Y", CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y)
    .value("MAX_GRID_DIM_Z", CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z)
    .value("MAX_SHARED_MEMORY_PER_BLOCK", CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK)
    .value("SHARED_MEMORY_PER_BLOCK", CU_DEVICE_ATTRIBUTE_SHARED_MEMORY_PER_BLOCK)
    .value("TOTAL_CONSTANT_MEMORY", CU_DEVICE_ATTRIBUTE_TOTAL_CONSTANT_MEMORY)
    .value("WARP_SIZE", CU_DEVICE_ATTRIBUTE_WARP_SIZE)
    .value("MAX_PITCH", CU_DEVICE_ATTRIBUTE_MAX_PITCH)
    .value("MAX_REGISTERS_PER_BLOCK", CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK)
    .value("REGISTERS_PER_BLOCK", CU_DEVICE_ATTRIBUTE_REGISTERS_PER_BLOCK)
    .value("CLOCK_RATE", CU_DEVICE_ATTRIBUTE_CLOCK_RATE)
    .value("TEXTURE_ALIGNMENT", CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT)

    .value("GPU_OVERLAP", CU_DEVICE_ATTRIBUTE_GPU_OVERLAP)
    .value("MULTIPROCESSOR_COUNT", CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT)
    ;
  py::enum_<CUmemorytype>("memorytype")
    .value("HOST", CU_MEMORYTYPE_HOST)
    .value("DEVICE", CU_MEMORYTYPE_DEVICE)
    .value("ARRAY", CU_MEMORYTYPE_ARRAY)
    ;

  py::def("init", init,
      py::arg("flags")=0);

  {
    typedef device cl;
    py::class_<cl>("Device", py::no_init)
      .def("__init__", py::make_constructor(make_device))
      .DEF_SIMPLE_METHOD(count)
      .staticmethod("count")
      .DEF_SIMPLE_METHOD(name)
      .DEF_SIMPLE_METHOD(compute_capability)
      .DEF_SIMPLE_METHOD(total_memory)
      .DEF_SIMPLE_METHOD(get_attribute)
      .def("make_context", &cl::make_context, 
          py::return_value_policy<py::manage_new_object>(),
          (py::args("self"), py::args("flags")=CU_CTX_SCHED_AUTO)
          )
      ;
  }

  {
    typedef context cl;
    py::class_<cl>("Context", py::no_init)
      .DEF_SIMPLE_METHOD(detach)
      .DEF_SIMPLE_METHOD(push)
      .DEF_SIMPLE_METHOD(pop)
      .DEF_SIMPLE_METHOD(get_device)
      .staticmethod("get_device")
      .DEF_SIMPLE_METHOD(synchronize)
      .staticmethod("synchronize")
      ;
  }

  {
    typedef stream cl;
    py::class_<cl, boost::noncopyable>
      ("Stream", py::init<py::optional<unsigned int> >(py::arg("flags")))
      .DEF_SIMPLE_METHOD(synchronize)
      .DEF_SIMPLE_METHOD(is_done)
      ;
  }

  {
    typedef module cl;
    py::class_<cl, boost::noncopyable>("Module", py::no_init)
      .def("get_function", &cl::get_function, (py::args("self", "name")))
      .def("get_global", &cl::get_global, (py::args("self", "name")))
      ;
  }
  py::def("load_module", load_module,
      py::return_value_policy<py::manage_new_object>());
  py::def("module_from_buffer", module_from_buffer,
      py::return_value_policy<py::manage_new_object>());

  {
    typedef function cl;
    py::class_<cl>("Function", py::no_init)
      .DEF_SIMPLE_METHOD(set_block_shape)
      .DEF_SIMPLE_METHOD(set_shared_size)
      .DEF_SIMPLE_METHOD(param_set_size)
      .def("param_seti", (void (cl::*)(int, unsigned int)) &cl::param_set)
      .def("param_setf", (void (cl::*)(int, float )) &cl::param_set)
      .DEF_SIMPLE_METHOD(param_setv)

      .DEF_SIMPLE_METHOD(launch)
      .DEF_SIMPLE_METHOD(launch_grid)
      .DEF_SIMPLE_METHOD(launch_grid_async)
      ;
  }

  {
    typedef device_allocation cl;
    py::class_<cl, boost::noncopyable>("DeviceAllocation", py::no_init)
      .def("__int__", &cl::operator CUdeviceptr)
      ;

    py::implicitly_convertible<device_allocation, CUdeviceptr>();
  }

  DEF_SIMPLE_FUNCTION(mem_get_info);
  py::def("mem_alloc", mem_alloc, py::return_value_policy<py::manage_new_object>());
  DEF_SIMPLE_FUNCTION(mem_alloc_pitch);
  DEF_SIMPLE_FUNCTION(mem_get_address_range);

  py::def("memset_d8", cuMemsetD8, py::args("dest", "data", "size"));
  py::def("memset_d16", cuMemsetD16, py::args("dest", "data", "size"));
  py::def("memset_d32", cuMemsetD32, py::args("dest", "data", "size"));

  py::def("memset_d2d8", cuMemsetD2D8, 
      py::args("dest", "pitch", "data", "width", "height"));
  py::def("memset_d2d16", cuMemsetD2D16, 
      py::args("dest", "pitch", "data", "width", "height"));
  py::def("memset_d2d32", cuMemsetD2D32, 
      py::args("dest", "pitch", "data", "width", "height"));

  py::def("memcpy_htod", memcpy_htod, py::args("dest", "src"));
  py::def("memcpy_dtoh", memcpy_dtoh, py::args("dest", "src"));
  py::def("memcpy_htod", memcpy_htod_async, py::args("dest", "src", "stream"));
  py::def("memcpy_dtoh", memcpy_dtoh_async, py::args("dest", "src", "stream"));
  py::def("memcpy_dtod", cuMemcpyDtoD, py::args("dest", "src", "size"));

  py::def("pagelocked_empty", pagelocked_empty,
      (py::arg("shape"), py::arg("dtype"), py::arg("order")="C"));

  {
    typedef event cl;
    py::class_<cl, boost::noncopyable>
      ("Event", py::init<py::optional<unsigned int> >(py::arg("flags")))
      .DEF_SIMPLE_METHOD(record)
      .def("record", &cl::record_in_stream)
      .DEF_SIMPLE_METHOD(synchronize)
      .DEF_SIMPLE_METHOD(query)
      .DEF_SIMPLE_METHOD(time_since)
      .DEF_SIMPLE_METHOD(time_till)
      ;
  }
}
