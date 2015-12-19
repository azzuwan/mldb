/** tensorflow_plugin.cc
    Jeremy Barnes, 24 November 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

*/

#include "mldb/core/mldb_entity.h"
#include "mldb/core/function.h"
#include "mldb/core/plugin.h"
#include "mldb/types/structure_description.h"
#include "mldb/arch/timers.h"

//#include "tensorflow/cc/ops/const_op.h"
//#include "tensorflow/cc/ops/image_ops.h"
//#include "tensorflow/cc/ops/standard_ops.h"

#if 0
namespace tensorflow {
using std::string;
} // namespace tensorflow
#endif

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_def.pb.h"
#include "tensorflow/core/platform/init_main.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/public/session.h"

#include "tensorflow/cc/ops/const_op.h"
#include "tensorflow/cc/ops/image_ops.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/public/tensor.h"
#include "tensorflow/core/graph/default_device.h"
#include "tensorflow/core/graph/graph_def_builder.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/logging.h"

using namespace std;

// Plugin entry point.  This is called by MLDB once the plugin is loaded.
// We initialize the TensorFlow system.

Datacratic::MLDB::Plugin *
mldbPluginEnterV100(Datacratic::MLDB::MldbServer * server)
{
    using namespace Datacratic;
    using namespace Datacratic::MLDB;

    int argc = 0;
    char ** argv = new char * [2];
    argv[0] = strdup("myprogram");
    argv[1] = nullptr;

    cerr << "Initializing TensorFlow" << endl;
    tensorflow::port::InitMain(argv[0], &argc, &argv);

    using namespace tensorflow;
    
    bool include_internal = true;
    OpList ops;
    OpRegistry::Global()->Export(include_internal, &ops);
    
    cerr << "there are " << ops.op_size() << " ops registered" << endl;
    for (unsigned i = 0;  i < ops.op_size();  ++i) {
        cerr << ops.op(i).name() << " " << ops.op(i).summary() << endl;
    }

    string graph_file_name = "inception/tensorflow_inception_graph.pb";

    tensorflow::GraphDef graph_def;
    Status load_graph_status =
        ReadBinaryProto(tensorflow::Env::Default(), graph_file_name, &graph_def);
    if (!load_graph_status.ok()) {
        throw HttpReturnException(500, "Couldn't load Inception model");
    }

    std::unique_ptr<Session> session(tensorflow::NewSession(tensorflow::SessionOptions()));
    Status session_create_status = session->Create(graph_def);
    
    if (!session_create_status.ok()) {
        throw HttpReturnException(500, "Couldn't initialize Inception session");
    }

    Tensor resizedImage;
    
    {

        std::string file_name = "ext/tensorflow/tensorflow/examples/label_image/data/grace_hopper.jpg";

        int input_width = 299;
        int input_height = 299;
        float input_mean = 128;
        float input_std = 128;

        tensorflow::GraphDefBuilder b;
        string input_name = "file_reader";
        string output_name = "normalized";
        tensorflow::Node* file_reader =
            tensorflow::ops::ReadFile(tensorflow::ops::Const(StringPiece(file_name), b.opts()),
                                      b.opts().WithName(input_name));

        // Now try to figure out what kind of file it is and decode it.
        const int wanted_channels = 3;
        tensorflow::Node* image_reader;
        if (tensorflow::StringPiece(file_name).ends_with(".png")) {
            image_reader = tensorflow::ops::DecodePng(
                                                      file_reader,
                                                      b.opts().WithAttr("channels", wanted_channels).WithName("png_reader"));
        } else {
            // Assume if it's not a PNG then it must be a JPEG.
            image_reader = tensorflow::ops::DecodeJpeg(
                                                       file_reader,
                                                       b.opts().WithAttr("channels", wanted_channels).WithName("jpeg_reader"));
        }
        // Now cast the image data to float so we can do normal math on it.
        tensorflow::Node* float_caster = tensorflow::ops::Cast(
                                                               image_reader, tensorflow::DT_FLOAT, b.opts().WithName("float_caster"));
        // The convention for image ops in TensorFlow is that all images are expected
        // to be in batches, so that they're four-dimensional arrays with indices of
        // [batch, height, width, channel]. Because we only have a single image, we
        // have to add a batch dimension of 1 to the start with ExpandDims().
        tensorflow::Node* dims_expander = tensorflow::ops::ExpandDims(
                                                                      float_caster, tensorflow::ops::Const(0, b.opts()), b.opts());
        // Bilinearly resize the image to fit the required dimensions.
        tensorflow::Node* resized = tensorflow::ops::ResizeBilinear(
                                                                    dims_expander, tensorflow::ops::Const({input_height, input_width},
                                                                                                          b.opts().WithName("size")),
                                                                    b.opts());
        // Subtract the mean and divide by the scale.
        tensorflow::ops::Div(
                             tensorflow::ops::Sub(
                                                  resized, tensorflow::ops::Const({input_mean}, b.opts()), b.opts()),
                             tensorflow::ops::Const({input_std}, b.opts()),
                             b.opts().WithName(output_name));

        // This runs the GraphDef network definition that we've just constructed, and
        // returns the results in the output tensor.
        tensorflow::GraphDef graph;
        auto graphRes = b.ToGraphDef(&graph);
        if (!graphRes.ok())
            throw HttpReturnException(400, "Unable to construct the graph: "
                                      + graphRes.error_message());

        std::unique_ptr<Session> session(tensorflow::NewSession(tensorflow::SessionOptions()));

        auto createRes = session->Create(graph);
        if (!createRes.ok())
            throw HttpReturnException(400, "Unable to create graph: " + createRes.error_message());

        std::vector<Tensor> out_tensors;
        auto runRes = session->Run({}, {output_name}, {}, &out_tensors);
        if (!runRes.ok())
            throw HttpReturnException(400, "Unable to run output tensors: " + runRes.error_message());

        cerr << "returned " << out_tensors.size() << " tensors" << endl;

        resizedImage = std::move(out_tensors.at(0));
    }

    string input_layer = "Mul";
    string output_layer = "softmax";

    // Actually run the image through the model.
    Tensor output;

    ML::Timer timer;

    for (unsigned i = 0;  i < 20;  ++i) {

        std::vector<Tensor> outputs;
        Status run_status = session->Run({{input_layer, resizedImage}},
                                         {output_layer}, {}, &outputs);

        if (!run_status.ok()) {
            throw HttpReturnException(400, "Unable to run model: "
                                      + run_status.error_message());
        }

        cerr << "outputs " << outputs.size() << " tensors" << endl;

        output = std::move(outputs.at(0));
    }

    cerr << "elapsed " << timer.elapsed() << endl;

    auto scores = output.flat<float>();

    vector<pair<float, int> > sorted;
    for (unsigned i = 0;  i < scores.size();  ++i)
        sorted.emplace_back(scores(i), i);

    std::sort(sorted.begin(), sorted.end());
    std::reverse(sorted.begin(), sorted.end());

    for (unsigned i = 0;  i < 5 && i < sorted.size();  ++i) {
        cerr << "category " << sorted[i].second << " score " << sorted[i].first
             << endl;
    }

#if 0
    cerr << "output tensor has " << output.shape().dims() << " dims" << endl;
    for (unsigned i = 0;  i < output.shape().dims();  ++i) {
        cerr << "dim " << i << " has value " << output.shape().dim_size(i)
             << endl;
    }
#endif
    
    return nullptr;
}


namespace Datacratic {
namespace MLDB {

const Package & tensorflowPackage()
{
    static const Package result("tensorflow");
    return result;
}


/*****************************************************************************/
/* TENSORFLOW KERNEL                                                         */
/*****************************************************************************/

struct TensorflowKernelConfig {
};

DECLARE_STRUCTURE_DESCRIPTION(TensorflowKernelConfig);

DEFINE_STRUCTURE_DESCRIPTION(TensorflowKernelConfig);

TensorflowKernelConfigDescription::
TensorflowKernelConfigDescription()
{
}

struct TensorflowKernel: public Function {

    TensorflowKernelConfig functionConfig;

    TensorflowKernel(MldbServer * owner,
                     PolyConfig config,
                     const std::function<bool (const Json::Value &)> & onProgress)
        : Function(owner)
    {
        functionConfig = config.params.convert<TensorflowKernelConfig>();   
    }

    Any getStatus() const
    {
        return Any();
    }

    FunctionOutput
    apply(const FunctionApplier & applier,
          const FunctionContext & context) const
    {
        FunctionOutput result;

        Utf8String output("output");
        result.set("output", ExpressionValue("hello", Date::notADate()));
    
        return result;
    }

    FunctionInfo
    getFunctionInfo() const
    {
        FunctionInfo result;

        result.input.addAtomValue("text");
        result.output.addAtomValue("output");
    
        return result;
    }

};


} // namespace MLDB
} // namespace Datacratic