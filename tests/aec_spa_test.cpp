#include <spa/interfaces/audio/aec.h>
#include <spa/support/plugin.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <array>
int child(const char* adapter,const char* runtime,int delay){
 void* library=dlopen(adapter,RTLD_NOW|RTLD_LOCAL);if(!library)return 2;
 auto enumerate=reinterpret_cast<spa_handle_factory_enum_func_t>(dlsym(library,SPA_HANDLE_FACTORY_ENUM_FUNC_NAME));
 auto health=reinterpret_cast<bool(*)()>(dlsym(library,"hyprcapture_aec_unhealthy"));if(!enumerate||!health)return 3;
 uint32_t index=0;const spa_handle_factory* factory=nullptr;if(enumerate(&factory,&index)!=1)return 4;
 auto* handle=static_cast<spa_handle*>(std::calloc(1,spa_handle_factory_get_size(factory,nullptr)));
 if(spa_handle_factory_init(factory,handle,nullptr,nullptr,0)<0)return 5;
 spa_audio_aec* aec=nullptr;if(spa_handle_get_interface(handle,SPA_TYPE_INTERFACE_AUDIO_AEC,reinterpret_cast<void**>(&aec))<0)return 6;
 spa_dict_item items[]={{"dtln.model","256"},{"dtln.backend","cpu"},{"dtln.directory","/unused-test-model"},{"dtln.cpu-library",runtime}};
 spa_dict args=SPA_DICT_INIT(items,4);spa_audio_info_raw rec{},out{},play{};
 rec.rate=out.rate=play.rate=48000;rec.channels=out.channels=play.channels=1;
 if(spa_audio_aec_init2(aec,&args,&rec,&out,&play)<0)return 7;
 setenv("AEC_TEST_DELAY_MS",std::to_string(delay).c_str(),1); // only the fake runtime reads this
 std::array<float,384> mic{},ref{},output{};const float* m[]={mic.data()};const float* r[]={ref.data()};float* o[]={output.data()};
 const auto start=std::chrono::steady_clock::now();
 for(int frame=0;frame<400;++frame){
  spa_audio_aec_run(aec,m,r,o,384);
  if(health())return 0;
  std::this_thread::sleep_until(start+std::chrono::milliseconds(8*(frame+1)));
 }
 return 8;
}
int main(int argc,char** argv){
 if(argc!=3)return 2;
 for(int delay:{20,20000}){
  const auto p=fork();if(p==0)_exit(child(argv[1],argv[2],delay));
  int status=0;waitpid(p,&status,0);
  if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){std::cerr<<"SPA overload failed "<<delay<<" status "<<status<<'\n';return 1;}
 }
 std::cout<<"SPA sustained overload and hung inference detected from audio callback\n";
}
