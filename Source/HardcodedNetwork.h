/*
    Copyright (c) 2015 Peter Rudenko

    Permission is hereby granted, free of charge, to any person obtaining
    a copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the Software
    is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
    OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
    OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifndef TINYRNN_HARDCODEDNETWORK_H_INCLUDED
#define TINYRNN_HARDCODEDNETWORK_H_INCLUDED

#include "Common.h"
#include "HardcodedNeuron.h"
#include "HardcodedTrainingContext.h"
#include "ScopedTimer.h"
#include "ScopedMemoryBlock.h"
#include "SerializedObject.h"
#include "Id.h"

namespace TinyRNN
{
    class HardcodedNetwork final : public SerializedObject
    {
    public:
        
        using Ptr = std::shared_ptr<HardcodedNetwork>;
        using HardcodedLayers = std::vector<HardcodedNeuron::Vector>;
        using StandaloneSources = std::map<std::string, std::string>;
        
    public:
        
        explicit HardcodedNetwork(HardcodedTrainingContext::Ptr targetContext);
        
        HardcodedNetwork(HardcodedTrainingContext::Ptr targetContext,
                         HardcodedLayers targetLayers,
                         unsigned long maxNumberOfExpressionsPerKernel);
        
        bool compile();
        StandaloneSources asStandalone(const std::string &name, bool asConst) const;
        
        HardcodedTrainingContext::Ptr getContext() const noexcept;
        
        HardcodedTrainingContext::RawData feed(const HardcodedTrainingContext::RawData &values);
        void train(Value rate, const HardcodedTrainingContext::RawData &target);
        
    public:
        
        virtual void deserialize(SerializationContext::Ptr context) override;
        virtual void serialize(SerializationContext::Ptr context) const override;
        
    private:
        
        HardcodedTrainingContext::Ptr trainingContext;
        
    private:
        
#if TINYRNN_OPENCL_ACCELERATION
        
        cl::Device clDevice;
        cl::Context clContext;
        cl::Program clProgram;
        cl::CommandQueue clQueue;
        
        cl::Buffer clMemoryBuffer;
        cl::Buffer clInputsBuffer;
        cl::Buffer clOutputsBuffer;
        cl::Buffer clTargetsBuffer;
        cl::Buffer clRateBuffer;
        
#endif
        
        class Kernel final : public SerializedObject
        {
        public:
            
            using Ptr = std::shared_ptr<Kernel>;
            using Vector = std::vector<Kernel::Ptr>;
            
            Kernel() : isBuilt(false), numExpressions(0) {}
            
            bool isBuilt;
            size_t numExpressions;
            std::string fullSource;
            std::string entryPoint;
            
#if TINYRNN_OPENCL_ACCELERATION
            cl::Kernel clKernel;
#endif
            
        public:
            
            virtual void deserialize(SerializationContext::Ptr context) override;
            virtual void serialize(SerializationContext::Ptr context) const override;
            
        private:
            
            TINYRNN_DISALLOW_COPY_AND_ASSIGN(Kernel);
        };
        
        Kernel::Vector feedKernels;
        Kernel::Vector trainKernels;
        
        Kernel::Vector compileFeedKernels(const HardcodedLayers &targetLayers,
                                          unsigned long maxNumberOfExpressionsPerKernel) const;
        
        Kernel::Vector compileTrainKernels(const HardcodedLayers &targetLayers,
                                           unsigned long maxNumberOfExpressionsPerKernel) const;
        
        bool initialize(const HardcodedLayers &targetLayers,
                        unsigned long maxNumberOfExpressionsPerKernel);
        
        bool isBuilt() const;
        
        TINYRNN_DISALLOW_COPY_AND_ASSIGN(HardcodedNetwork);
    };
    
    //===------------------------------------------------------------------===//
    // HardcodedNetwork implementation
    //===------------------------------------------------------------------===//
    
    inline HardcodedNetwork::HardcodedNetwork(HardcodedTrainingContext::Ptr targetContext) :
    trainingContext(targetContext)
    {
        HardcodedLayers empty;
        this->initialize(empty, 0);
    }

    inline HardcodedNetwork::HardcodedNetwork(HardcodedTrainingContext::Ptr targetContext,
                                              HardcodedLayers targetLayers,
                                              unsigned long maxNumberOfExpressionsPerKernel) :
    trainingContext(targetContext)
    {
        this->initialize(targetLayers, maxNumberOfExpressionsPerKernel);
    }
    
    inline HardcodedTrainingContext::Ptr HardcodedNetwork::getContext() const noexcept
    {
        return this->trainingContext;
    }
    
    //===------------------------------------------------------------------===//
    // Compiling
    //===------------------------------------------------------------------===//
    
    inline bool HardcodedNetwork::initialize(const HardcodedLayers &targetLayers,
                                             unsigned long maxNumberOfExpressionsPerKernel)
    {
        const ScopedTimer timer("HardcodedNetwork::initialize");
        
#if TINYRNN_OPENCL_ACCELERATION
        
        std::vector<cl::Platform> allPlatforms;
        cl::Platform::get(&allPlatforms);
        
        if (allPlatforms.size() == 0)
        {
            std::cout << "No OpenCL platforms found!\n";
            return false;
        }
        
        const cl::Platform defaultPlatform = allPlatforms.front();
        std::cout << "OpenCL platform: " << defaultPlatform.getInfo<CL_PLATFORM_NAME>() << "\n";
        
        std::vector<cl::Device> allDevices;
        defaultPlatform.getDevices(CL_DEVICE_TYPE_ALL, &allDevices);
        
        if (allDevices.empty())
        {
            std::cout << "No OpenCL devices found!\n";
            return false;
        }
        
        this->clDevice = allDevices.front();
        std::cout << "Using OpenCL device: " << this->clDevice.getInfo<CL_DEVICE_NAME>() << "\n";
        
        this->clContext = cl::Context(this->clDevice);
        
#endif
        
        this->feedKernels = this->compileFeedKernels(targetLayers, maxNumberOfExpressionsPerKernel);
        this->trainKernels = this->compileTrainKernels(targetLayers, maxNumberOfExpressionsPerKernel);
        
        return true;
    }
    
    inline bool HardcodedNetwork::compile()
    {
        const ScopedTimer timer("HardcodedNetwork::build");
        
#if TINYRNN_OPENCL_ACCELERATION
        
        cl::Program::Sources clSources;
        
        for (const auto &kernel : this->feedKernels)
        {
            clSources.push_back({kernel->fullSource.c_str(), kernel->fullSource.length()});
        }
        
        for (const auto &kernel : this->trainKernels)
        {
            clSources.push_back({kernel->fullSource.c_str(), kernel->fullSource.length()});
        }
        
        this->clProgram = cl::Program(this->clContext, clSources);
        
        if (this->clProgram.build({this->clDevice}) != CL_SUCCESS)
        {
            std::cout << " Error building: " << this->clProgram.getBuildInfo<CL_PROGRAM_BUILD_LOG>(this->clDevice) << std::endl;
            return false;
        }
        else
        {
            std::cout << "Build ok, variables count: " << this->trainingContext->getMemory().size() << std::endl;
        }
        
        for (auto &kernel : this->feedKernels)
        {
            kernel->clKernel = cl::Kernel(this->clProgram, kernel->entryPoint.c_str());
            kernel->isBuilt = true;
        }
        
        for (auto &kernel : this->trainKernels)
        {
            kernel->clKernel = cl::Kernel(this->clProgram, kernel->entryPoint.c_str());
            kernel->isBuilt = true;
        }

        this->clQueue = cl::CommandQueue(this->clContext, this->clDevice);
        
        this->clMemoryBuffer = cl::Buffer(this->clContext,
                                          CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                                          sizeof(Value) * this->trainingContext->getMemory().size(),
                                          (void *)this->trainingContext->getMemory().data());
        
        return true;
        
#else
        
        return false;
        
#endif
    }
    
    static std::string valueString()
    {
        return (sizeof(Value) == sizeof(double)) ? "double" : "float";
    }
    
    inline HardcodedNetwork::StandaloneSources HardcodedNetwork::asStandalone(const std::string &name, bool asConst) const
    {
        StandaloneSources result;
        
        const bool isTrainable = (!this->trainKernels.empty());
        
        if (this->feedKernels.empty())
        {
            return result;
        }
        
        const std::string headerName = name + ".h";
        const std::string sourceName = name + ".c";
        const std::string feedEntry = name + "Feed";
        const std::string trainEntry = name + "Train";
        const std::string networkGuard = "TINYRNN_STANDALONE_GUARD_" + Uuid::generateIsoUuid();
        
        std::stringstream header;
        
        header << "#ifndef " << networkGuard << std::endl;
        header << "#define " << networkGuard << std::endl;
        
        header << std::endl;
        header << "extern " + valueString() + " kMemory[];" << std::endl;
        header << "const int kMemorySize = " << this->trainingContext->getMemory().size() << ";" << std::endl;
        header << std::endl;
        header << "extern " + valueString() + " kOutputs[];" << std::endl;
        header << "const int kOutputsSize = " << this->trainingContext->getOutputs().size() << ";" << std::endl;
        header << std::endl;
        
        header << "void " << feedEntry << "(const " << valueString() << " *input);" << std::endl;
        
        if (! asConst)
        {
            header << "void " << trainEntry << "(const " << valueString() << " rate, const " << valueString() << " *target);" << std::endl;
        }
        
        header << std::endl;
        
        header << "#endif //" << networkGuard << std::endl;
        
        std::stringstream source;
        
        source << "#include \"" << headerName << "\"" <<  std::endl;
        source << "#define kernel" << std::endl;
        source << "#define global" << std::endl;
        source << std::endl;
        
        source << "static " << this->feedKernels.front()->fullSource;
        source << std::endl;
        
        if (! asConst)
        {
            source << "static " << this->trainKernels.front()->fullSource;
            source << std::endl;
        }
        
        source << "void " << feedEntry << "(const " << valueString() << " *input) {" << std::endl;
        source << "    " << this->feedKernels.front()->entryPoint << "(input, kOutputs, kMemory);" << std::endl;
        source << "}" << std::endl;
        source << std::endl;
        
        if (! asConst)
        {
            source << "void " << trainEntry << "(const " << valueString() << " rate, const " << valueString() << " *target) {" << std::endl;
            source << "    " << this->trainKernels.front()->entryPoint << "(&rate, target, kMemory);" << std::endl;
            source << "}" << std::endl;
            source << std::endl;
        }
        
        const int linebreakEveryNth = 8;
        
        source << valueString() + " kMemory[] = " << std::endl;
        source << "{ " << std::endl;
        
        for (size_t i = 0; i < this->trainingContext->getMemory().size(); ++i)
        {
            source << this->trainingContext->getMemory().at(i) << ", ";
            if (i % linebreakEveryNth == (linebreakEveryNth - 1)) { source << std::endl; }
        }
        
        source << "0 }; " << std::endl << std::endl;
        
        source << valueString() + " kOutputs[] = " << std::endl;
        source << "{ " << std::endl;
        
        for (size_t i = 0; i < this->trainingContext->getOutputs().size(); ++i)
        {
            source << this->trainingContext->getOutputs().at(i) << ", ";
            if (i % linebreakEveryNth == (linebreakEveryNth - 1)) { source << std::endl; }
        }
        
        source << "0 }; " << std::endl;
        
        result[name + ".h"] = header.str();
        result[name + ".c"] = source.str();
        return result;
    }
    
    inline bool HardcodedNetwork::isBuilt() const
    {
        bool built = (!this->feedKernels.empty() &&
                      !this->trainKernels.empty());
        
        for (auto &kernel : this->feedKernels)
        {
            built = built && kernel->isBuilt;
        }
        
        for (auto &kernel : this->trainKernels)
        {
            built = built && kernel->isBuilt;
        }
        
        return built;
    }
    
    //===------------------------------------------------------------------===//
    // Compiling all the expressions
    //===------------------------------------------------------------------===//
    
#define trnn_max(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a > _b ? _a : _b; })
    
    inline HardcodedNetwork::Kernel::Vector
    HardcodedNetwork::compileFeedKernels(const HardcodedLayers &targetLayers,
                                         unsigned long maxNumberOfExpressionsPerKernel) const
    {
        Kernel::Vector result;
        Kernel::Ptr currentKernel;
        size_t currentKernelExpressionsCounter = 0;
        const size_t maxExpressions = trnn_max(maxNumberOfExpressionsPerKernel, 100);
        
        for (const auto &layer : targetLayers)
        {
            for (const auto &neuron : layer)
            {
                const KernelSentence &feedChunk = neuron->getFeedChunk();
                const KernelSentence &traceChunk = neuron->getTraceChunk();
                const size_t numExpressionsToCome = (feedChunk.getSize() + traceChunk.getSize());
                const bool shouldStartNewKernel =
                (currentKernel == nullptr) ||
                ((currentKernelExpressionsCounter + numExpressionsToCome) >= maxExpressions);
                
                if (shouldStartNewKernel)
                {
                    if (currentKernel != nullptr)
                    {
                        currentKernel->fullSource += this->trainingContext->buildOutputsExpressions();
                        currentKernel->fullSource += "}\n";
                        //std::cout << "Adding a feed kernel of " << currentKernel->numExpressions << " lines." << std::endl;
                        result.push_back(currentKernel);
                        currentKernelExpressionsCounter = 0;
                    }
                    
                    currentKernel = Kernel::Ptr(new Kernel());
                    currentKernel->entryPoint = ("feed_" + std::to_string(result.size()));
                    currentKernel->fullSource =
                    "void kernel " + currentKernel->entryPoint +
                    "(global const " + valueString() +
                    " *input, global " + valueString() +
                    " *output, global " + valueString() + " *x) {\n";
                    
                    currentKernel->fullSource += this->trainingContext->buildInputsExpressions();
                }
                
                currentKernelExpressionsCounter += numExpressionsToCome;
                currentKernel->numExpressions = currentKernelExpressionsCounter;
                currentKernel->fullSource += (feedChunk.build() + traceChunk.build());
            }
        }
        
        if (currentKernel != nullptr)
        {
            currentKernel->fullSource += this->trainingContext->buildOutputsExpressions();
            currentKernel->fullSource += "}\n";
            //std::cout << "Adding a feed kernel of " << currentKernel->numExpressions << " lines." << std::endl;
            result.push_back(currentKernel);
        }
        
        return result;
    }
    
    inline HardcodedNetwork::Kernel::Vector
    HardcodedNetwork::compileTrainKernels(const HardcodedLayers &targetLayers,
                                          unsigned long maxNumberOfExpressionsPerKernel) const
    {
        Kernel::Vector result;
        Kernel::Ptr currentKernel;
        size_t currentKernelExpressionsCounter = 0;
        const size_t maxExpressions = trnn_max(maxNumberOfExpressionsPerKernel, 100);
        
        for (size_t l = targetLayers.size(); l --> 0 ;)
        {
            const auto &layer = targetLayers[l];
            
            for (size_t n = layer.size(); n --> 0 ;)
            {
                const auto &neuron = layer[n];
                const KernelSentence &trainChunk = neuron->getTrainChunk();
                const size_t numExpressionsToCome = trainChunk.getSize();
                const bool shouldStartNewKernel =
                (currentKernel == nullptr) ||
                ((currentKernelExpressionsCounter + numExpressionsToCome) > maxExpressions);
                
                if (shouldStartNewKernel)
                {
                    if (currentKernel != nullptr)
                    {
                        currentKernel->fullSource += "}\n";
                        //std::cout << "Adding a train kernel of " << currentKernel->numExpressions << " lines." << std::endl;
                        result.push_back(currentKernel);
                        currentKernelExpressionsCounter = 0;
                    }
                    
                    currentKernel = Kernel::Ptr(new Kernel());
                    currentKernel->entryPoint = ("train_" + std::to_string(result.size()));
                    currentKernel->fullSource =
                    "void kernel " + currentKernel->entryPoint +
                    "(global const " + valueString() +
                    " *rate, global const " + valueString() +
                    " *target, global " + valueString() + " *x) {\n";
                    
                    currentKernel->fullSource += this->trainingContext->buildRateExpression();
                    currentKernel->fullSource += this->trainingContext->buildTargetsExpressions();
                    
                }
                
                currentKernelExpressionsCounter += numExpressionsToCome;
                currentKernel->numExpressions = currentKernelExpressionsCounter;
                currentKernel->fullSource += trainChunk.build();
            }
        }
        
        if (currentKernel != nullptr)
        {
            currentKernel->fullSource += "}\n";
            //std::cout << "Adding a train kernel of " << currentKernel->numExpressions << " lines." << std::endl;
            result.push_back(currentKernel);
        }
        
        return result;
    }
    
    //===------------------------------------------------------------------===//
    // Core
    //===------------------------------------------------------------------===//
    
    inline HardcodedTrainingContext::RawData HardcodedNetwork::feed(const HardcodedTrainingContext::RawData &inputs)
    {
#if TINYRNN_OPENCL_ACCELERATION
        
        std::fill(this->trainingContext->getOutputs().begin(),
                  this->trainingContext->getOutputs().end(),
                  0.0);
        
        this->clInputsBuffer = cl::Buffer(this->clContext,
                                          CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                                          sizeof(Value) * inputs.size(),
                                          (void *)inputs.data());
        
        this->clOutputsBuffer = cl::Buffer(this->clContext,
                                           CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
                                           sizeof(Value) * this->trainingContext->getOutputs().size(),
                                           (void *)this->trainingContext->getOutputs().data());
        
        for (auto &kernel : this->feedKernels)
        {
            kernel->clKernel.setArg(0, this->clInputsBuffer);
            kernel->clKernel.setArg(1, this->clOutputsBuffer);
            kernel->clKernel.setArg(2, this->clMemoryBuffer);
            this->clQueue.enqueueNDRangeKernel(kernel->clKernel, cl::NullRange, cl::NDRange(1), cl::NullRange);
            this->clQueue.finish();
        }
        
        return this->trainingContext->getOutputs();
        
#else
        
        return {};
        
#endif
    }
    
    inline void HardcodedNetwork::train(Value rate, const HardcodedTrainingContext::RawData &targets)
    {
#if TINYRNN_OPENCL_ACCELERATION
        
        this->clTargetsBuffer = cl::Buffer(this->clContext,
                                           CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                                           sizeof(Value) * targets.size(),
                                           (void *)targets.data());
        
        this->clRateBuffer = cl::Buffer(this->clContext,
                                        CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                                        sizeof(Value),
                                        (void *)&rate);
        
        for (auto &kernel : this->trainKernels)
        {
            kernel->clKernel.setArg(0, this->clRateBuffer);
            kernel->clKernel.setArg(1, this->clTargetsBuffer);
            kernel->clKernel.setArg(2, this->clMemoryBuffer);
            this->clQueue.enqueueNDRangeKernel(kernel->clKernel, cl::NullRange, cl::NDRange(1), cl::NullRange);
            this->clQueue.finish();
        }
        
#endif
    }
    
    //===------------------------------------------------------------------===//
    // Serialization
    //===------------------------------------------------------------------===//
    
    inline void HardcodedNetwork::deserialize(SerializationContext::Ptr context)
    {
        this->feedKernels.clear();
        this->trainKernels.clear();
        
        SerializationContext::Ptr feedKernelsNode(context->getChildContext(Keys::Hardcoded::FeedKernels));
        
        for (size_t i = 0; i < feedKernelsNode->getNumChildrenContexts(); ++i)
        {
            SerializationContext::Ptr kernelNode(feedKernelsNode->getChildContext(i));
            Kernel::Ptr kernel(new Kernel());
            kernel->deserialize(kernelNode);
            this->feedKernels.push_back(kernel);
        }
        
        SerializationContext::Ptr trainKernelsNode(context->getChildContext(Keys::Hardcoded::TrainKernels));
        
        for (size_t i = 0; i < trainKernelsNode->getNumChildrenContexts(); ++i)
        {
            SerializationContext::Ptr kernelNode(trainKernelsNode->getChildContext(i));
            Kernel::Ptr kernel(new Kernel());
            kernel->deserialize(kernelNode);
            this->trainKernels.push_back(kernel);
        }
    }
    
    inline void HardcodedNetwork::serialize(SerializationContext::Ptr context) const
    {
        SerializationContext::Ptr feedKernelsNode(context->createChildContext(Keys::Hardcoded::FeedKernels));
        
        for (const auto &kernel : this->feedKernels)
        {
            SerializationContext::Ptr kernelNode(feedKernelsNode->createChildContext(Keys::Hardcoded::FeedKernel));
            kernel->serialize(kernelNode);
        }
        
        SerializationContext::Ptr trainKernelsNode(context->createChildContext(Keys::Hardcoded::TrainKernels));
        
        for (const auto &kernel : this->trainKernels)
        {
            SerializationContext::Ptr kernelNode(trainKernelsNode->createChildContext(Keys::Hardcoded::TrainKernel));
            kernel->serialize(kernelNode);
        }
        
        // TODO also save the compiled program binaries, if any
//        context->setNumberProperty(this->isBuilt(), Keys::Hardcoded::IsBuilt);
//        
//        if (this->isBuilt())
//        {
//            SerializationContext::Ptr binariesNode(context->createChildContext(Keys::Hardcoded::KernelBinaries));
//            const std::vector<size_t> sizes = this->clProgram.getInfo<CL_PROGRAM_BINARY_SIZES>();
//            const std::vector<char *> binaries = this->clProgram.getInfo<CL_PROGRAM_BINARIES>();
//            
//            for (size_t i = 0; i < binaries.size(); ++i)
//            {
//                SerializationContext::Ptr binaryNode(binariesNode->createChildContext(Keys::Hardcoded::KernelBinary));
//                const size_t size = sizes[i];
//                const std::string binary(context->encodeBase64((unsigned const char *)binaries[i], size));
//                
//                //std::cout << binary << std::endl;
//                // todo base64encode?
//                
//                //binaryNode->setStringProperty(binary, Keys::Hardcoded::Content);
//            }
//        }
    }
    
    inline void HardcodedNetwork::Kernel::deserialize(SerializationContext::Ptr context)
    {
        this->numExpressions = context->getNumberProperty(Keys::Hardcoded::NumExpressions);
        this->entryPoint = context->getStringProperty(Keys::Hardcoded::EntryPoint);
        this->fullSource = context->getStringProperty(Keys::Hardcoded::FullSource);
    }
    
    inline void HardcodedNetwork::Kernel::serialize(SerializationContext::Ptr context) const
    {
        context->setNumberProperty(this->numExpressions, Keys::Hardcoded::NumExpressions);
        context->setStringProperty(this->entryPoint, Keys::Hardcoded::EntryPoint);
        context->setStringProperty(this->fullSource, Keys::Hardcoded::FullSource);
    }
}

#endif // TINYRNN_HARDCODEDNETWORK_H_INCLUDED
