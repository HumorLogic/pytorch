#include <torch/csrc/python_headers.h>

#include <torch/csrc/distributed/rpc/process_group_agent.h>
#include <torch/csrc/distributed/rpc/py_rref.h>
#include <torch/csrc/distributed/rpc/python_functions.h>
#include <torch/csrc/distributed/rpc/python_rpc_handler.h>
#include <torch/csrc/distributed/rpc/rpc_agent.h>
#include <torch/csrc/distributed/rpc/rref.h>
#include <torch/csrc/distributed/rpc/rref_context.h>
#include <torch/csrc/distributed/rpc/script_functions.h>
#include <torch/csrc/distributed/rpc/types.h>
#include <torch/csrc/jit/pybind_utils.h>
#include <torch/csrc/utils/future.h>
#include <torch/csrc/utils/object_ptr.h>
#include <torch/csrc/utils/pybind.h>
#include <torch/types.h>

#include <pybind11/chrono.h>

namespace torch {
namespace distributed {
namespace rpc {

namespace {

template <typename T>
using shared_ptr_class_ = py::class_<T, std::shared_ptr<T>>;

PyObject* rpc_init(PyObject* /* unused */) {
  auto rpc_module =
      THPObjectPtr(PyImport_ImportModule("torch.distributed.rpc"));
  if (!rpc_module) {
    throw python_error();
  }

  auto module = py::handle(rpc_module).cast<py::module>();

  auto workerInfo = shared_ptr_class_<WorkerInfo>(module, "WorkerInfo")
                        .def_readonly("name", &WorkerInfo::name_)
                        .def_readonly("id", &WorkerInfo::id_);

  auto rpcAgent =
      shared_ptr_class_<RpcAgent>(module, "RpcAgent")
          .def(
              "join", &RpcAgent::join, py::call_guard<py::gil_scoped_release>())
          .def(
              "sync",
              &RpcAgent::sync,
              py::call_guard<py::gil_scoped_release>());

  auto pyRRef =
      shared_ptr_class_<PyRRef>(module, "RRef", R"(
          A class encapsulating a reference to a value of some type on a remote worker.
          This handle will keep the referenced remote value alive on the worker.
      )")
          .def(py::init<const py::object&>())
          .def(
              // not releasing GIL here to avoid context switch on getters
              "is_owner",
              &PyRRef::isOwner)
          .def(
              // not releasing GIL here to avoid context switch on getters
              "owner",
              &PyRRef::owner)
          .def(
              "to_here",
              &PyRRef::toHere,
              py::call_guard<py::gil_scoped_release>())
          .def(
              "local_value",
              &PyRRef::localValue,
              py::call_guard<py::gil_scoped_release>())
          .def(py::pickle(
              [](const PyRRef& self) {
                // __getstate__
                return self.pickle();
              },
              [](py::tuple t) { // NOLINT
                // __setstate__
                return PyRRef::unpickle(t);
              }));

  // future.wait() should not be called after join_rpc(), e.g., pythonRpcHandler
  // is cleaned up in join_rpc(), after join_rpc(), python objects returned
  // from rpc python call can not be resolved.
  auto futureMessage =
      shared_ptr_class_<torch::utils::Future<Message>>(module, "FutureMessage")
          .def(
              "wait",
              [&](torch::utils::Future<Message>& fut) {
                return toPyObj(fut.wait());
              },
              py::call_guard<py::gil_scoped_release>());

  shared_ptr_class_<ProcessGroupAgent>(module, "ProcessGroupAgent", rpcAgent)
      .def(
          py::init<
              std::string,
              std::shared_ptr<::c10d::ProcessGroup>,
              int,
              std::chrono::milliseconds>(),
          py::arg("name"),
          py::arg("process_group"),
          py::arg("num_send_recv_threads"),
          py::arg("rpc_timeout"))
      .def(
          "get_worker_info",
          (const WorkerInfo& (ProcessGroupAgent::*)(void)const) &
              RpcAgent::getWorkerInfo,
          py::call_guard<py::gil_scoped_release>())
      .def(
          "get_worker_info",
          (const WorkerInfo& (ProcessGroupAgent::*)(const std::string&)const) &
              ProcessGroupAgent::getWorkerInfo,
          py::call_guard<py::gil_scoped_release>())
      .def(
          "join",
          &ProcessGroupAgent::join,
          py::call_guard<py::gil_scoped_release>())
      .def(
          "sync",
          &ProcessGroupAgent::sync,
          py::call_guard<py::gil_scoped_release>());

  module.def("_start_rpc_agent", [](const std::shared_ptr<RpcAgent>& agent) {
    RpcAgent::setDefaultRpcAgent(agent);
    agent->start();
  });

  module.def("_destroy_rref_context", []() {
    RRefContext::getInstance().destroyInstance();
  });

  module.def("_cleanup_python_rpc_handler", []() {
    PythonRpcHandler::getInstance().cleanup();
  });

  module.def(
      "_invoke_rpc_builtin",
      [](RpcAgent& agent,
         const WorkerInfo& dst,
         const std::string& opName,
         const py::args& args,
         const py::kwargs& kwargs) {
        return pyRpcBuiltin(agent, dst, opName, args, kwargs);
      });

  module.def(
      "_invoke_rpc_python_udf",
      [](RpcAgent& agent,
         const WorkerInfo& dst,
         std::string& pickledPythonUDF,
         std::vector<torch::Tensor>& tensors) {
        return pyRpcPythonUdf(agent, dst, pickledPythonUDF, tensors);
      });

  struct PythonFutureWrapper {
    explicit PythonFutureWrapper(c10::intrusive_ptr<c10::ivalue::Future> fut)
        : fut(std::move(fut)) {}

    c10::intrusive_ptr<c10::ivalue::Future> fut;
  };

  py::class_<PythonFutureWrapper>(module, "PythonFutureWrapper")
      .def("wait", [](PythonFutureWrapper& fut) {
        fut.fut->wait();
        auto res = fut.fut->value();
        {
          // acquiring GIL as torch::jit::toPyObject creates new py::object
          // without grabbing the GIL.
          AutoGIL ag;
          return torch::jit::toPyObject(std::move(res));
        }
      });

  module.def(
      "_invoke_rpc_script",
      [](const std::string& dst,
         const std::string& qualifiedName,
         const py::args& args,
         const py::kwargs& kwargs) {
        // No need to catch exception here, if function can not be found,
        // exception will be thrown in get_function() call; if args do not match
        // with function schema, exception will be thrown in
        // createStackForSchema() call.
        auto name = c10::QualifiedName(qualifiedName);
        auto fnSchema = PythonRpcHandler::getInstance()
                            .jitCompilationUnit()
                            ->get_function(name)
                            .getSchema();
        auto stack = torch::jit::createStackForSchema(
            fnSchema, args, kwargs, c10::nullopt);
        auto fut = rpcTorchscriptCall(dst, name, stack);
        return PythonFutureWrapper(fut.toFuture());
      });

  module.def(
      "_invoke_remote_builtin",
      [](RpcAgent& agent,
         const WorkerInfo& dst,
         const std::string& opName,
         const py::args& args,
         const py::kwargs& kwargs) {
        return pyRemoteBuiltin(agent, dst, opName, args, kwargs);
      });

  module.def(
      "_invoke_remote_python_udf",
      [](RpcAgent& agent,
         const WorkerInfo& dst,
         std::string& pickledPythonUDF,
         std::vector<torch::Tensor>& tensors) {
        return pyRemotePythonUdf(agent, dst, pickledPythonUDF, tensors);
      });

  module.def(
      "get_rpc_timeout",
      []() { return RpcAgent::getDefaultRpcAgent()->getRpcTimeout(); },
      R"(
          Retrieve the timeout for all RPCs that was set during RPC initialization.

          Returns:
            `datetime.timedelta` instance indicating the RPC timeout.
      )");

  Py_RETURN_TRUE;
}

} // namespace

static PyMethodDef methods[] = { // NOLINT
    {"_rpc_init", (PyCFunction)rpc_init, METH_NOARGS, nullptr},
    {nullptr, nullptr, 0, nullptr}};

PyMethodDef* python_functions() {
  return methods;
}

} // namespace rpc
} // namespace distributed
} // namespace torch
