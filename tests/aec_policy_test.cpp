#include "audio/aec_policy.hpp"
#include "ui/remembered_settings.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>
#include <QJsonDocument>
#include <iostream>
#include <cstdlib>
#include <sched.h>
using namespace hyprcapture;
void require(bool v,const char* m){if(!v){std::cerr<<m<<'\n';std::exit(1);}}
int main(int argc,char** argv){
    QTemporaryDir root; require(root.isValid(),"isolated storage");
    qputenv("XDG_CACHE_HOME",root.path().toUtf8());qputenv("XDG_CONFIG_HOME",root.path().toUtf8());
    qputenv("HYPRCAPTURE_AEC_MODEL_DIR",(root.path()+"/models").toUtf8());
    qputenv("HYPRCAPTURE_TFLITE_LIBRARY",(root.path()+"/runtime.so").toUtf8());
    QCoreApplication app(argc,argv);namespace aec=audio::aec;
    require(aec::selection(-1,"cpu")["aec"]=="pending","new machine starts pending");
    require(aec::selection(0,"cpu")["aec"]=="off","forced off needs no runtime");
    require(aec::writeCache("cpu",{{"status","complete"},{"reason","insufficient performance"}}),"save cache");
    require(!aec::readCache("cpu").isEmpty(),"reuse same fingerprint");
    require(aec::readCache("npu").isEmpty(),"backend cache isolated");
    QFile f(aec::cachePath("cpu"));require(f.open(QIODevice::ReadOnly),"read cache");
    auto data=f.readAll();f.close();QFile id("/etc/machine-id");
    if(id.open(QIODevice::ReadOnly)){auto raw=id.readAll().trimmed();require(raw.isEmpty()||!data.contains(raw),"raw machine identity not persisted");}
    auto value=QJsonDocument::fromJson(data).object();value["fingerprint"]="other-machine";
    require(f.open(QIODevice::WriteOnly|QIODevice::Truncate),"tamper cache");f.write(QJsonDocument(value).toJson());f.close();
    require(aec::readCache("cpu").isEmpty(),"copied cache invalidated");
    require(aec::writeCache("cpu",{{"status","pending"},{"reason","computer busy"}}),"busy cache");
    require(aec::selection(-1,"cpu")["aec"]=="pending","busy is retryable");
    const auto before=aec::fingerprint("cpu");QFile runtime(root.path()+"/runtime.so");
    require(runtime.open(QIODevice::WriteOnly),"runtime fixture");runtime.write("changed");runtime.close();
    require(before!=aec::fingerprint("cpu"),"runtime change invalidates fingerprint");
    cpu_set_t original;CPU_ZERO(&original);
    if(sched_getaffinity(0,sizeof(original),&original)==0&&CPU_COUNT(&original)>1){
        cpu_set_t single;CPU_ZERO(&single);for(int i=0;i<CPU_SETSIZE;++i)if(CPU_ISSET(i,&original)){CPU_SET(i,&single);break;}
        auto old=aec::fingerprint("cpu");require(sched_setaffinity(0,sizeof(single),&single)==0,"set test affinity");
        require(old!=aec::fingerprint("cpu"),"available CPU count invalidates fingerprint");
        require(sched_setaffinity(0,sizeof(original),&original)==0,"restore affinity");
    }
    CaptureDefaults explicitOff;explicitOff.recordAudioEchoCancellation=0;ui::saveAecPreferences(explicitOff);
    CaptureDefaults migrated;ui::restoreAecPreferences(migrated);require(migrated.recordAudioEchoCancellation==0,"manual off survives independent of cache");
    CaptureDefaults explicitOn;explicitOn.recordAudioEchoCancellation=1;ui::restoreAecPreferences(explicitOn);require(explicitOn.recordAudioEchoCancellation==1,"explicit config overrides stored off");
    QSettings legacy(root.path()+"/hyprcapture/last-settings.ini",QSettings::IniFormat);legacy.setValue("version",1);legacy.setValue("recordAudioEchoCancellation",false);legacy.sync();
    QFile::remove(root.path()+"/hyprcapture/aec.ini");CaptureDefaults old;old.rememberSettings=true;ui::restoreAecPreferences(old);require(ui::restoreSettings(old),"load valid legacy settings");
    require(old.recordAudioEchoCancellation==-1,"legacy ambiguous bool migrates to automatic");
    std::cout<<"AEC cache identity, pending state, runtime change, affinity and explicit preferences passed\n";
}
