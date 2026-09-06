#include "audio/aec_policy.hpp"
#include "audio/aec_fixture.hpp"
#include "audio/dtln.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QTimer>
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <dlfcn.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <csignal>

using namespace hyprcapture::audio;
namespace {
using Clock = std::chrono::steady_clock;
void print(QJsonObject o) { const auto b=QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n'; (void)!write(STDOUT_FILENO,b.constData(),b.size()); }
std::unique_ptr<dtln::Engine> engine(int size, const QString& backend) {
    if (backend != "cpu" && backend != "npu") throw std::runtime_error("Unsupported AEC backend");
    QString error; if (!aec::modelsValid(size,&error)) throw std::runtime_error(error.toStdString());
    return std::make_unique<dtln::Engine>(backend=="npu" ? dtln::npuNetwork(aec::modelDirectory().toStdString(),aec::nativeComponent("libhyprcapture-aec-openvino.so").toStdString(),size)
        : dtln::cpuNetwork(aec::modelDirectory().toStdString(),aec::runtimeLibrary().toStdString(),size),size);
}
void prime(dtln::Engine& e, int frames) { std::array<float,128> z{},o{}; for(int i=0;i<frames;++i)e.process(z.data(),z.data(),o.data()); }
std::vector<float> readFloats(const QString& path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)||f.size()%4||f.size()>1024LL*1024*1024)throw std::runtime_error("Invalid PCM input");
    auto data=f.readAll(); std::vector<float> result(data.size()/4);std::memcpy(result.data(),data.data(),data.size());return result;
}
bool busy() {
    auto read=[] { QFile f("/proc/stat");std::array<double,2> r{};if(f.open(QIODevice::ReadOnly)){auto v=f.readLine().simplified().split(' ');for(int i=1;i<std::min(qsizetype(9),v.size());++i)r[0]+=v[i].toDouble();r[1]=v.value(4).toDouble()+v.value(5).toDouble();}return r;};
    auto a=read();std::this_thread::sleep_for(std::chrono::milliseconds(200));auto b=read();
    return b[0]>a[0]&&(b[1]-a[1])/(b[0]-a[0])<.4;
}
QJsonObject benchmark(int size,const QString& backend) {
    auto e=engine(size,backend); prime(*e,250);e->reset();prime(*e,3);
    QFile file(QString(":/aec/golden-%1.qz").arg(size));
    if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("AEC reference data missing");
    auto data=qUncompress(file.readAll());
    // Full ten-second output followed by both recurrent states for every frame.
    const size_t samples=160000, stateCount=(samples/128)*2*4*size;
    if(data.size()!=qsizetype((samples+stateCount)*sizeof(float)))throw std::runtime_error("Invalid AEC reference data");
    std::vector<float> golden(samples+stateCount);std::memcpy(golden.data(),data.data(),data.size());
    std::vector<float> mic,ref;aec::fixture(mic,ref);std::array<float,128> out{};
    QJsonArray runs;bool correct=true,fast=true;
    for(int trial=0;trial<2;++trial){
        e->reset();prime(*e,3);double xx=0,yy=0,xy=0,err=0,stateErr=0,statePower=0;std::vector<double> times;
        const auto start=Clock::now();
        for(size_t offset=0;offset<samples;offset+=128){
            auto t=Clock::now();e->process(mic.data()+offset,ref.data()+offset,out.data());
            times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-t).count());
            for(int j=0;j<128;++j){double x=golden[offset+j],y=out[j];xx+=x*x;yy+=y*y;xy+=x*y;err+=(x-y)*(x-y);}
            for(int p=0;p<2;++p){
                const auto& state=e->state(p);size_t base=samples+((offset/128)*2+p)*4*size;
                for(size_t j=0;j<state.size();++j){double x=golden[base+j],y=state[j];stateErr+=(x-y)*(x-y);statePower+=x*x;}
            }
        }
        (void)start;
        // Measure the real 48 kHz path, including both fixed resamplers and all
        // buffering. Golden validation above independently covers inference.
        e->reset();prime(*e,3);dtln::Stream48 stream(*e);
        std::array<float,384> mic48{},ref48{},out48{};times.clear();
        const auto chainStart=Clock::now();
        for(size_t offset=0;offset<samples;offset+=128){
            for(int j=0;j<384;++j){mic48[j]=mic[offset+j/3];ref48[j]=ref[offset+j/3];}
            auto t=Clock::now();stream.process(mic48.data(),ref48.data(),out48.data());
            times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-t).count());
            for(float v:out48)if(!std::isfinite(v))throw std::runtime_error("Invalid resampled output");
        }
        const double elapsed=std::chrono::duration<double>(Clock::now()-chainStart).count();
        std::sort(times.begin(),times.end());const double p99=times[size_t(.99*(times.size()-1))];
        const double correlation=xy/std::sqrt(std::max(1e-30,xx*yy)),relative=std::sqrt(err/std::max(xx,1e-30));
        const double stateRelative=std::sqrt(stateErr/std::max(statePower,1e-30));
        const bool valid=std::isfinite(relative)&&correlation>(backend=="npu"?.98:.999)&&relative<(backend=="npu"?.20:.03)&&stateRelative<(backend=="npu"?.10:.01);
        correct &= valid;fast &= elapsed<=10./3.&&p99<=4.;
        runs.append(QJsonObject{{"seconds",elapsed},{"rtf",elapsed/10.},{"p99Ms",p99},{"maxMs",times.back()},{"correlation",correlation},{"relativeError",relative},{"stateRelativeError",stateRelative}});
    }
    return {{"correct",correct},{"fast",fast&&correct},{"runs",runs},{"runtime",QString::fromStdString(e->version())}};
}
QJsonObject check(const QString& backend,bool force) {
    auto cached=aec::readCache(backend);if(!force&&!cached.isEmpty()&&cached["status"]!="pending")return cached;
    QDir().mkpath(QFileInfo(aec::cachePath(backend)).absolutePath());QLockFile lock(aec::cachePath(backend)+".lock");
    if(!lock.tryLock())return {{"status","pending"},{"reason","test already running"}};
    if(busy()){QJsonObject r{{"status","pending"},{"reason","computer busy"}};aec::writeCache(backend,r);return r;}
    QJsonObject models;bool anyCorrect=false,anyFast=false;
    for(int size:{512,256}){
        try {auto r=benchmark(size,backend);models[QString::number(size)]=r;anyCorrect|=r["correct"].toBool();if(r["fast"].toBool()){anyFast=true;break;}}
        catch(const std::exception& e){models[QString::number(size)]=QJsonObject{{"correct",false},{"fast",false},{"error",e.what()}};}
    }
    QJsonObject result{{"status",anyCorrect?"complete":"pending"},{"models",models},{"reason",anyFast?"":anyCorrect?"insufficient performance":"runtime or correctness check unavailable"},{"testedAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    // Contended tests are not durable evidence of a slow machine.
    if(busy()){result["status"]="pending";result["reason"]="computer busy";}
    aec::writeCache(backend,result);return result;
}
bool installModels(QString& error) {
    if(!QDir().mkpath(aec::modelDirectory())){error="Cannot create model directory";return false;}
    QNetworkAccessManager network;
    for(const auto& item:aec::modelFiles){
        const int size=QString(item.name).contains("512")?512:256;
        if(aec::modelsValid(size))continue;
        QNetworkRequest request(QUrl(QString("https://raw.githubusercontent.com/breizhn/DTLN-aec/9d24e128b4f409db18227b8babb343016625921f/pretrained_models/")+item.name));
        request.setTransferTimeout(60000);auto* reply=network.get(request);QEventLoop loop;QTimer deadline;deadline.setSingleShot(true);
        QObject::connect(reply,&QNetworkReply::finished,&loop,&QEventLoop::quit);QObject::connect(&deadline,&QTimer::timeout,reply,&QNetworkReply::abort);
        deadline.start(120000);loop.exec();
        auto bytes=reply->readAll();bool ok=reply->error()==QNetworkReply::NoError&&bytes.size()<32*1024*1024&&QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()==item.sha256;
        reply->deleteLater();if(!ok){error="Model download or checksum failed";return false;}
        QSaveFile file(aec::modelDirectory()+"/"+item.name);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){error="Cannot install model";return false;}
    }
    return true;
}
int serve(QCoreApplication& app,const QStringList& args){
    if(args.size()!=7)return 2;
    const auto mic=args[2],output=args[3],name=args[6],backend=args[5];const int size=args[4].toInt();
    if (backend != "cpu" && backend != "npu") throw std::runtime_error("Unsupported AEC backend");
    QString error;if(!aec::modelsValid(size,&error))throw std::runtime_error(error.toStdString());
    const auto library=aec::nativeComponent("libspa-aec-dtln.so");
    const auto spaDirectory=qEnvironmentVariable("SPA_PLUGIN_DIR",HYPRCAPTURE_SPA_PLUGIN_DIR);
    qputenv("SPA_PLUGIN_DIR",(QFileInfo(library).absolutePath()+":"+spaDirectory).toUtf8());
    pw_init(nullptr,nullptr);auto* loop=pw_main_loop_new(nullptr);
    auto* context=loop?pw_context_new(pw_main_loop_get_loop(loop),pw_properties_new(PW_KEY_CONFIG_NAME,"client-rt.conf",nullptr),0):nullptr;
    if(!context)throw std::runtime_error("PipeWire initialization failed");
    void* handle=dlopen(library.toUtf8().constData(),RTLD_NOW|RTLD_LOCAL);
    auto health=handle?reinterpret_cast<bool(*)()>(dlsym(handle,"hyprcapture_aec_unhealthy")):nullptr;
    QJsonObject config{{"library.name","libspa-aec-dtln"},{"monitor.mode",true},{"audio.rate",48000},{"audio.position",QJsonArray{"MONO"}},
        {"aec.args",QJsonObject{{"dtln.directory",aec::modelDirectory()},{"dtln.cpu-library",aec::runtimeLibrary()},
            {"dtln.npu-library",aec::nativeComponent("libhyprcapture-aec-openvino.so")},{"dtln.backend",backend},{"dtln.model",QString::number(size)}}},
        {"capture.props",QJsonObject{{"target.object",mic},{"node.dont-fallback",true},{"node.dont-reconnect",true},{"node.passive",true}}},
        {"sink.props",QJsonObject{{"target.object",output},{"node.dont-fallback",true},{"node.dont-reconnect",true},{"node.passive",true}}},
        {"source.props",QJsonObject{{"node.name",name},{"node.description","HyprCapture DTLN microphone"},{"node.virtual",true},{"priority.session",0},{"priority.driver",0}}}};
    auto bytes=QJsonDocument(config).toJson(QJsonDocument::Compact);auto* module=pw_context_load_module(context,"libpipewire-module-echo-cancel",bytes.constData(),nullptr);
    if(!module||!health){if(module)pw_impl_module_destroy(module);pw_context_destroy(context);pw_main_loop_destroy(loop);if(handle)dlclose(handle);throw std::runtime_error("DTLN PipeWire module failed to start");}
    struct State{pw_impl_module* module;QCoreApplication* app;spa_hook listener{};}state{module,&app};
    static const pw_impl_module_events events{.version=PW_VERSION_IMPL_MODULE_EVENTS,.destroy=[](void* data){auto& s=*static_cast<State*>(data);s.module=nullptr;spa_hook_remove(&s.listener);s.app->exit(1);}};
    pw_impl_module_add_listener(module,&state.listener,&events,&state);
    QTimer pump;QObject::connect(&pump,&QTimer::timeout,[&]{if(pw_loop_iterate(pw_main_loop_get_loop(loop),0)<0||health()){print({{"error","AEC processing stopped or queue overloaded"}});app.exit(1);}});pump.start(2);
    QSocketNotifier stop(STDIN_FILENO,QSocketNotifier::Read);QObject::connect(&stop,&QSocketNotifier::activated,[&]{char b[64];if(read(0,b,sizeof(b))<=0)app.quit();});
    print({{"aec","active"},{"model",size},{"backend",backend},{"delayUs",aec::processingDelayUs}});
    const int result=app.exec();pump.stop();if(state.module){spa_hook_remove(&state.listener);pw_impl_module_destroy(state.module);}pw_context_destroy(context);pw_main_loop_destroy(loop);dlclose(handle);return result;
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("hyprcapture-aec");
    // No orphan processor after a crashed/killed recording helper.
    const auto parent=getppid();prctl(PR_SET_PDEATHSIG,SIGTERM);if(getppid()!=parent)return 1;
    const auto args=app.arguments();const auto command=args.value(1);const auto backend=args.contains("--npu")?QString("npu"):QString("cpu");
    try{
        if(command=="--status"){int policy=args.value(2,"-1").toInt();auto d=aec::selection(policy,backend);d["description"]=aec::description(d,policy);print(d);return 0;}
        if(command=="--check"||command=="--install"){
            if(command=="--install"){QString error;if(!installModels(error)){aec::writeCache(backend,{{"status","pending"},{"reason",error}});print({{"status","pending"},{"reason",error}});return 0;}}
            print(check(backend,args.contains("--force")));return 0;
        }
        if(command=="--serve")return serve(app,args);
        if((command=="--process16"||command=="--process48")&&args.size()==7){
            auto mic=readFloats(args[2]),ref=readFloats(args[3]);const auto count=std::min(mic.size(),ref.size());auto e=engine(args[5].toInt(),args[6]);prime(*e,3);
            const size_t packet=command=="--process48"?384:128;dtln::Stream48 stream(*e);
            std::vector<float> output(count);std::array<float,384> in{},far{},out{};
            auto start=Clock::now();for(size_t offset=0;offset<count;offset+=packet){in.fill(0);far.fill(0);auto n=std::min(packet,count-offset);std::copy_n(mic.data()+offset,n,in.data());std::copy_n(ref.data()+offset,n,far.data());if(packet==384)stream.process(in.data(),far.data(),out.data());else e->process(in.data(),far.data(),out.data());std::copy_n(out.data(),n,output.data()+offset);}
            QFile f(args[4]);if(!f.open(QIODevice::WriteOnly)||f.write(reinterpret_cast<const char*>(output.data()),output.size()*4)!=qint64(output.size()*4))return 1;
            print({{"seconds",std::chrono::duration<double>(Clock::now()-start).count()},{"audioSeconds",double(count)/(packet==384?48000.:16000.)},{"backend",args[6]}});return 0;
        }
        print({{"error","Usage: hyprcapture-aec --install|--check [--force] [--npu]; --status -1|0|1 [--npu]"}});return 2;
    }catch(const std::exception& e){print({{"error",e.what()}});return 1;}
}
