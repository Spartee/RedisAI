#include "torch_c.h"
#include "torch/torch.h"
#include "redismodule.h"
#include "ATen/Functions.h"
#include "torch/csrc/jit/serialization/import.h"
#include "torch/csrc/jit/api/compilation_unit.h"

#include <iostream>
#include <sstream>

#include "torch_extensions/torch_redis.h"
namespace {

static DLDataType getDLDataType(const at::Tensor &t) {
    DLDataType dtype;
    dtype.lanes = 1;
    dtype.bits = t.element_size() * 8;
    switch (t.scalar_type()) {
    case at::ScalarType::Byte:
        dtype.code = DLDataTypeCode::kDLUInt;
        break;
    case at::ScalarType::Char:
        dtype.code = DLDataTypeCode::kDLInt;
        break;
    case at::ScalarType::Double:
        dtype.code = DLDataTypeCode::kDLFloat;
        break;
    case at::ScalarType::Float:
        dtype.code = DLDataTypeCode::kDLFloat;
        break;
    case at::ScalarType::Int:
        dtype.code = DLDataTypeCode::kDLInt;
        break;
    case at::ScalarType::Long:
        dtype.code = DLDataTypeCode::kDLInt;
        break;
    case at::ScalarType::Short:
        dtype.code = DLDataTypeCode::kDLInt;
        break;
    case at::ScalarType::Half:
        dtype.code = DLDataTypeCode::kDLFloat;
        break;
    case at::ScalarType::Bool:
        throw std::logic_error("Bool is not supported by dlpack");
    case at::ScalarType::BFloat16:
        throw std::logic_error("BFloat16 is not supported by dlpack");
    case at::ScalarType::QInt8:
        throw std::logic_error("QInt8 is not supported by dlpack");
    case at::ScalarType::QUInt8:
        throw std::logic_error("QUInt8 is not supported by dlpack");
    case at::ScalarType::QInt32:
        throw std::logic_error("QInt32 is not supported by dlpack");
    case at::ScalarType::ComplexHalf:
        throw std::logic_error("ComplexHalf is not supported by dlpack");
    case at::ScalarType::ComplexFloat:
        throw std::logic_error("ComplexFloat is not supported by dlpack");
    case at::ScalarType::ComplexDouble:
        throw std::logic_error("ComplexDouble is not supported by dlpack");
    case at::ScalarType::Undefined:
        throw std::logic_error("Undefined is not a valid ScalarType");
    case at::ScalarType::NumOptions:
        throw std::logic_error("NumOptions is not a valid ScalarType");
    }
    return dtype;
}

static DLDevice getDLDevice(const at::Tensor &tensor, const int64_t &device_id) {
    DLDevice device;
    device.device_id = device_id;
    if (tensor.is_cuda()) {
        device.device_type = DLDeviceType::kDLGPU;
    } else {
        device.device_type = DLDeviceType::kDLCPU;
    }
    return device;
}

static at::DeviceType getATenDeviceType(DLDeviceType device_type) {
    switch (device_type) {
    case DLDeviceType::kDLCPU:
        return at::DeviceType::CPU;
    case DLDeviceType::kDLGPU:
        return at::DeviceType::CUDA;
    case DLDeviceType::kDLOpenCL:
        return at::DeviceType::OPENCL;
    case DLDeviceType::kDLROCM:
        return at::DeviceType::HIP;
    default:
        throw std::logic_error("Unsupported device_type: " + std::to_string(device_type));
    }
    return at::DeviceType::CPU; // impossible
}

at::ScalarType toScalarType(const DLDataType &dtype) {
    at::ScalarType stype;
    if (dtype.lanes != 1)
        throw std::logic_error("ATen does not support lanes != 1");
    switch (dtype.code) {
    case DLDataTypeCode::kDLUInt:
        switch (dtype.bits) {
        case 8:
            stype = at::ScalarType::Byte;
            break;
        default:
            throw std::logic_error("Unsupported kUInt bits " + std::to_string(dtype.bits));
        }
        break;
    case DLDataTypeCode::kDLInt:
        switch (dtype.bits) {
        case 8:
            stype = at::ScalarType::Char;
            break;
        case 16:
            stype = at::ScalarType::Short;
            break;
        case 32:
            stype = at::ScalarType::Int;
            break;
        case 64:
            stype = at::ScalarType::Long;
            break;
        default:
            throw std::logic_error("Unsupported kInt bits " + std::to_string(dtype.bits));
        }
        break;
    case DLDataTypeCode::kDLFloat:
        switch (dtype.bits) {
        case 16:
            stype = at::ScalarType::Half;
            break;
        case 32:
            stype = at::ScalarType::Float;
            break;
        case 64:
            stype = at::ScalarType::Double;
            break;
        default:
            throw std::logic_error("Unsupported kFloat bits " + std::to_string(dtype.bits));
        }
        break;
    default:
        throw std::logic_error("Unsupported code " + std::to_string(dtype.code));
    }
    return stype;
}

torch::Tensor fromDLPack(const DLTensor *src) {
    at::DeviceType device_type = getATenDeviceType(src->device.device_type);
    at::ScalarType stype = toScalarType(src->dtype);
    // torch::Device device(device_type, src->ctx.device_id);
    torch::Device device(device_type, -1);
    // torch::DeviceType device = device_type;
    return torch::from_blob(src->data, at::IntArrayRef(src->shape, src->ndim),
                            at::IntArrayRef(src->strides, src->ndim),
                            torch::device(device).dtype(stype));
}

struct ATenDLMTensor {
    torch::Tensor handle;
    DLManagedTensor tensor;
};

void deleter(DLManagedTensor *arg) {
    delete static_cast<ATenDLMTensor *>(arg->manager_ctx);
    RedisModule_Free(arg); 
}

DLManagedTensor *toManagedDLPack(const torch::Tensor &src_) {
    ATenDLMTensor *atDLMTensor(new ATenDLMTensor);
    atDLMTensor->handle = src_;
    auto &src = atDLMTensor->handle;
    atDLMTensor->tensor.manager_ctx = atDLMTensor;
    atDLMTensor->tensor.deleter = &deleter;
    atDLMTensor->tensor.dl_tensor.data = src.data_ptr();
    int64_t device_id = 0;
    if (src.is_cuda()) {
        device_id = src.get_device();
    }
    atDLMTensor->tensor.dl_tensor.device = getDLDevice(src, device_id);
    atDLMTensor->tensor.dl_tensor.ndim = src.dim();
    atDLMTensor->tensor.dl_tensor.dtype = getDLDataType(src);
    atDLMTensor->tensor.dl_tensor.shape = const_cast<int64_t *>(src.sizes().data());
    atDLMTensor->tensor.dl_tensor.strides = const_cast<int64_t *>(src.strides().data());
    atDLMTensor->tensor.dl_tensor.byte_offset = 0;
    return &(atDLMTensor->tensor);
}

struct ModuleContext {
    std::shared_ptr<torch::jit::script::Module> module;
    std::shared_ptr<torch::jit::script::CompilationUnit> cu;
    DLDeviceType device;
    int64_t device_id;
};

static void torchHandlOutputs(torch::jit::Stack& stack, const char* fnName, long nOutputs, DLManagedTensor **outputs) {
    torch::DeviceType output_device_type = torch::kCPU;
    torch::Device output_device(output_device_type, -1);

    if(nOutputs == 0) return;
    int count = 0;
    for (size_t i = 0; i < stack.size(); i++) {
        if (count > nOutputs - 1) {
            throw std::runtime_error(
                std::string("Function returned unexpected number of outputs - ") + fnName);
        }

        if (stack[i].isTensor()) {
            outputs[count++] = toManagedDLPack(stack[i].toTensor().contiguous().to(output_device));
        } else if (stack[i].isTensorList()) {
            auto list = stack[i].toTensorList();
            for (size_t j = 0; j < list.size(); j++) {
                outputs[count++] = toManagedDLPack(list.get(j).contiguous().to(output_device));
            }
        } else if (stack[i].isTuple()) {
            auto &elements = stack[i].toTuple()->elements();
            for (size_t j = 0; j < elements.size(); j++) {
                if (elements[j].isTensor()) {
                    outputs[count++] =
                        toManagedDLPack(elements[j].toTensor().contiguous().to(output_device));
                } else {
                    throw std::runtime_error(std::string("Function returned non-tensor values") +
                                             fnName);
                }
            }
        } else {
            throw std::runtime_error(std::string("Function returned non-tensor values") + fnName);
        }
    }

    if (count != nOutputs) {
        throw std::runtime_error(std::string("Function returned unexpected number of outputs - ") +
                                 fnName);
    }
}

void torchRunModule(ModuleContext *ctx, const char *fnName, torch::jit::Stack& stack, long nOutputs, DLManagedTensor **outputs){

    if (ctx->module) {
        torch::NoGradGuard guard;
        torch::jit::script::Method method = ctx->module->get_method(fnName);
        method.run(stack);
    } else {
        torch::NoGradGuard guard;
        torch::jit::Function &fn = ctx->cu->get_function(fnName);
        fn.run(stack);
    }

    torchHandlOutputs(stack, fnName, nOutputs, outputs);
}

} // namespace

extern "C" void torchBasicTest() {
    torch::Tensor mat = torch::rand({3, 3});
    std::cout << mat << std::endl;
}

extern "C" DLManagedTensor *torchNewTensor(DLDataType dtype, long ndims, int64_t *shape,
                                           int64_t *strides, char *data) {
    // at::DeviceType device_type = getATenDeviceType(kDLCPU);
    at::ScalarType stype = toScalarType(dtype);
    torch::Device device(getATenDeviceType(kDLCPU), -1);
    torch::Tensor tensor =
        torch::from_blob(data, at::IntArrayRef(shape, ndims), at::IntArrayRef(strides, ndims),
                         // torch::device(at::DeviceType::CPU).dtype(stype));
                         torch::device(device).dtype(stype));

    DLManagedTensor *dl_tensor = toManagedDLPack(tensor);

    return dl_tensor;
}

extern "C" void* torchCompileScript(const char* script, DLDeviceType device, int64_t device_id,
                                    char **error, void* (*alloc)(size_t))
{
  ModuleContext* ctx = new ModuleContext();
  ctx->device = device;
  ctx->device_id = device_id;
  try {
    auto cu = std::make_shared<torch::jit::script::CompilationUnit>();
    cu->define(
        c10::nullopt,
        script,
        torch::jit::script::redisResolver(),
        nullptr);
    auto aten_device_type = getATenDeviceType(device);
    
    if (aten_device_type == at::DeviceType::CUDA && !torch::cuda::is_available()) {
      throw std::logic_error("GPU requested but Torch couldn't find CUDA");
    }
    ctx->cu = cu;
    ctx->module = nullptr;

  }
  catch(std::exception& e) {
    size_t len = strlen(e.what()) +1;
    *error = (char*)alloc(len * sizeof(char));
    strcpy(*error, e.what());
    (*error)[len-1] = '\0';
    delete ctx;
    return NULL;
  }
  return ctx;
}

extern "C" void *torchLoadModel(const char *graph, size_t graphlen, DLDeviceType device,
                                int64_t device_id, char **error, void *(*alloc)(size_t)) {
    std::string graphstr(graph, graphlen);
    std::istringstream graph_stream(graphstr, std::ios_base::binary);
    ModuleContext *ctx = new ModuleContext();
    ctx->device = device;
    ctx->device_id = device_id;
    try {
        // TODO: move to device now
        auto module = std::make_shared<torch::jit::script::Module>(torch::jit::load(graph_stream));
        auto aten_device_type = getATenDeviceType(device);
        if (aten_device_type == at::DeviceType::CUDA && !torch::cuda::is_available()) {
            throw std::logic_error("GPU requested but Torch couldn't find CUDA");
        }
        torch::Device aten_device(aten_device_type, device_id);
        module->to(aten_device);
        ctx->module = module;
        ctx->cu = nullptr;
    } catch (std::exception &e) {
        size_t len = strlen(e.what()) + 1;
        *error = (char *)alloc(len * sizeof(char));
        strcpy(*error, e.what());
        (*error)[len - 1] = '\0';
        delete ctx;
        return NULL;
    }
    return ctx;
}

static torch::DeviceType getDeviceType(ModuleContext *ctx) {
    switch (ctx->device) {
        case kDLCPU:
            return torch::kCPU;
        case kDLGPU:
            return torch::kCUDA;
        default:
            throw std::runtime_error(std::string("Unsupported device ") + std::to_string(ctx->device));
    }
}

extern "C" bool torchMatchScriptSchema(size_t nArguments, long nInputs, TorchScriptFunctionArgumentType* argumentTypes, 
                                       size_t nlists, size_t nkeys,
                                       char **error) {
    char* buf; 
    int schemaListCount = 0;
    int schemaStringCount = 0;
    if(nInputs < nArguments) {
        asprintf(&buf, "Wrong number of inputs. Expected %ld but was %ld", nArguments, nInputs);
        goto cleanup;
    }
    for (size_t i = 0; i < nArguments; i++) {
        if(argumentTypes[i] == LIST) {
            schemaListCount++;
            continue;
        }
        if(argumentTypes[i] == STRING) {
            schemaStringCount++;
            continue;
        }
    } 
    if(schemaListCount != nlists) {
        asprintf(&buf, "Wrong number of lists. Expected %d but was %ld", schemaListCount, nlists);
        goto cleanup;
    }
    if(schemaStringCount != nkeys) {
        asprintf(&buf, "Wrong number of keys. Expected %d but was %ld", schemaStringCount, nkeys);
        goto cleanup;
    }

    return true;

    cleanup:
    *error = RedisModule_Strdup(buf);
    free(buf);
    return false;
}

extern "C" void torchRunScript(void *scriptCtx, const char *fnName, long nInputs,
                               DLManagedTensor **inputs, long nOutputs, DLManagedTensor **outputs,
                                size_t nArguments,
                                TorchScriptFunctionArgumentType* argumentTypes,
                                size_t* listSizes,
                                RedisModuleString **keys,
                               char **error, void *(*alloc)(size_t)) {
    ModuleContext *ctx = (ModuleContext *)scriptCtx;
    try {
        torch::DeviceType device_type = getDeviceType(ctx);
        torch::Device device(device_type, ctx->device_id);

        torch::jit::Stack stack;

        size_t listsIdx = 0;
        size_t inputsIdx = 0;
        size_t keysIdx = 0;
        for(size_t i= 0; i < nArguments; i++) {
            // In case of tensor.
            if(argumentTypes[i] == TENSOR) {
                DLTensor *input = &(inputs[inputsIdx++]->dl_tensor);
                torch::Tensor tensor = fromDLPack(input);
                stack.push_back(tensor.to(device));
            }
            else if(argumentTypes[i] == LIST){
                std::vector<torch::Tensor> args;
                size_t argumentSize = listSizes[listsIdx++];
                for (size_t j = 0; j < argumentSize; j++) {
                    DLTensor *input = &(inputs[inputsIdx++]->dl_tensor);
                    torch::Tensor tensor = fromDLPack(input);
                    tensor.to(device);
                    args.emplace_back(tensor);
                }
                stack.push_back(args);
            } else {
                const char* cstr = RedisModule_StringPtrLen(keys[keysIdx++], NULL);
                torch::string str = torch::string();
                stack.push_back(str);
            }
        }

        torchRunModule(ctx, fnName, stack, nOutputs, outputs);
    } catch (std::exception &e) {
        size_t len = strlen(e.what()) + 1;
        *error = (char *)alloc(len * sizeof(char));
        strcpy(*error, e.what());
        (*error)[len - 1] = '\0';
    }
}

extern "C" void torchRunModel(void *modelCtx, long nInputs, DLManagedTensor **inputs, long nOutputs,
                              DLManagedTensor **outputs, char **error, void *(*alloc)(size_t)) {
    ModuleContext *ctx = (ModuleContext *)modelCtx;
    try {
        torch::DeviceType device_type = getDeviceType(ctx);
        torch::Device device(device_type, ctx->device_id);

        torch::jit::Stack stack;
        for (int i = 0; i < nInputs; i++) {
            DLTensor *input = &(inputs[i]->dl_tensor);
            torch::Tensor tensor = fromDLPack(input);
            stack.push_back(tensor.to(device));
        }
        torchRunModule(ctx, "forward", stack, nOutputs, outputs);
    } catch (std::exception &e) {
        size_t len = strlen(e.what()) + 1;
        *error = (char *)alloc(len * sizeof(char));
        strcpy(*error, e.what());
        (*error)[len - 1] = '\0';
    }
}

extern "C" void torchSerializeModel(void *modelCtx, char **buffer, size_t *len, char **error,
                                    void *(*alloc)(size_t)) {
    ModuleContext *ctx = (ModuleContext *)modelCtx;
    std::ostringstream out;
    try {
        ctx->module->save(out);
        auto out_str = out.str();
        int size = out_str.size();
        *buffer = (char *)alloc(size);
        memcpy(*buffer, out_str.c_str(), size);
        *len = size;
    } catch (std::exception &e) {
        size_t len = strlen(e.what()) + 1;
        *error = (char *)alloc(len * sizeof(char));
        strcpy(*error, e.what());
        (*error)[len - 1] = '\0';
    }
}

extern "C" void torchDeallocContext(void *ctx) {
    ModuleContext *ctx_ = (ModuleContext *)ctx;
    if (ctx_) {
        delete ctx_;
    }
}

extern "C" void torchSetInterOpThreads(int num_threads, char **error, void *(*alloc)(size_t)) {
    int current_num_interop_threads = torch::get_num_interop_threads();
    if (current_num_interop_threads != num_threads) {
        try {
            torch::set_num_interop_threads(num_threads);
        } catch (std::exception) {
            std::string error_msg =
                "Cannot set number of inter-op threads after parallel work has started";
            size_t len = error_msg.length() + 1;
            *error = (char *)alloc(len * sizeof(char));
            strcpy(*error, error_msg.c_str());
            (*error)[len - 1] = '\0';
        }
    }
}

extern "C" void torchSetIntraOpThreads(int num_threads, char **error, void *(*alloc)(size_t)) {
    int current_num_threads = torch::get_num_threads();
    if (current_num_threads != num_threads) {
        try {
            torch::set_num_threads(num_threads);
        } catch (std::exception) {
            std::string error_msg =
                "Cannot set number of intra-op threads after parallel work has started";
            size_t len = error_msg.length() + 1;
            *error = (char *)alloc(len * sizeof(char));
            strcpy(*error, error_msg.c_str());
            (*error)[len - 1] = '\0';
        }
    }
}

extern "C" size_t torchModelNumInputs(void *modelCtx, char** error) {
    ModuleContext *ctx = (ModuleContext *)modelCtx;
    size_t ninputs = 0;
    try {
        const c10::FunctionSchema& schema =  ctx->module->get_method("forward").function().getSchema();
        // First argument is `self`
        ninputs =  schema.arguments().size() - 1;
    }
    catch(std::exception ex) {
        int printed = asprintf(error, "Erorr while trying to retrive model inputs number: %s", ex.what());
    }
    return ninputs;
}

static int getArgumentTensorCount(const c10::Argument& arg){
    switch (arg.type()->kind())
    {
    case c10::TypeKind::TensorType:
        return 1;
        break;
    case c10::TypeKind::TupleType: {
        int count = 0;
        for(auto const& obj: arg.type()->containedTypes()) {
            if(obj->kind() == c10::TypeKind::TensorType) {
                count++;
            }
        }
        return count;
    }
    case c10::TypeKind::ListType: {
        return arg.N().value();
    }
    
    default:
        return 0;
    }
}

static TorchScriptFunctionArgumentType  getArgumentType(const c10::Argument& arg){
    switch (arg.type()->kind())
    {
    case c10::TypeKind::TensorType:
        return TENSOR;
    case c10::TypeKind::TupleType: {
        return TUPLE;
    }
    case c10::TypeKind::ListType: {
        return LIST;
    }
    case c10::TypeKind::StringType: {
        return STRING;
    } 
    default:
        return UNKOWN;
    }
}

extern "C" size_t torchModelNumOutputs(void *modelCtx, char** error) {
    ModuleContext *ctx = (ModuleContext *)modelCtx;
    size_t noutputs = 0;
    try {
        const c10::FunctionSchema& schema =  ctx->module->get_method("forward").function().getSchema();
        for (auto const& arg :schema.returns()){
           noutputs += getArgumentTensorCount(arg);
        }
    }
    catch(std::exception ex) {
       int printed = asprintf(error, "Erorr while trying to retrive model outputs number: %s", ex.what());
    }
    return noutputs;
}

extern "C" const char* torchModelInputNameAtIndex(void* modelCtx, size_t index, char** error) {
    ModuleContext *ctx = (ModuleContext *)modelCtx;
    const char* ret = NULL;
    try {
        const c10::FunctionSchema& schema =  ctx->module->get_method("forward").function().getSchema();
        ret =  schema.arguments()[index + 1].name().c_str();
    }
    catch(std::exception ex) {
       int printed = asprintf(error, "Erorr while trying to retrive model intput at index %ld: %s", index, ex.what());
    }
    return ret;
}

extern "C" size_t torchScript_FunctionCount(void* scriptCtx) {
    ModuleContext *ctx = (ModuleContext *)scriptCtx;
    return ctx->cu->get_functions().size();
}

extern "C" const char* torchScript_FunctionName(void* scriptCtx, size_t fn_index) {
    ModuleContext *ctx = (ModuleContext *)scriptCtx;
    std::vector<torch::jit::Function*> functions = ctx->cu->get_functions();
    return functions[fn_index]->name().c_str();
}

extern "C" size_t torchScript_FunctionArgumentCount(void* scriptCtx, size_t fn_index) {
    ModuleContext *ctx = (ModuleContext *)scriptCtx;
    std::vector<torch::jit::Function*> functions = ctx->cu->get_functions();
    return functions[fn_index]->getSchema().arguments().size();
}

extern "C" TorchScriptFunctionArgumentType torchScript_FunctionArgumentype(void* scriptCtx, size_t fn_index, size_t arg_index) {
    ModuleContext *ctx = (ModuleContext *)scriptCtx;
    std::vector<torch::jit::Function*> functions = ctx->cu->get_functions();
    return getArgumentType(ctx->cu->get_functions()[fn_index]->getSchema().arguments()[arg_index]);
}
