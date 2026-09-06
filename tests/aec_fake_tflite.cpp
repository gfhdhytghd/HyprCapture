// Test-only C ABI runtime to exercise queue protection without a model/device.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>
#include <cstdlib>
struct Tensor { std::vector<float> data; };
struct Model { int part; };
struct Interpreter { int part; Tensor input[3],output[2]; };
extern "C" {
const char* TfLiteVersion(){return "2.14.0";}
Model* TfLiteModelCreateFromFile(const char* p){return new Model{std::strstr(p,"_1.tflite")?0:1};}
void TfLiteModelDelete(Model* p){delete p;}
void* TfLiteInterpreterOptionsCreate(){return new int;}
void TfLiteInterpreterOptionsDelete(int* p){delete p;}
void TfLiteInterpreterOptionsSetNumThreads(void*,int){}
Interpreter* TfLiteInterpreterCreate(Model* m,void*){
 auto* i=new Interpreter{m->part};const int n=m->part?512:257;
 i->input[0].data.resize(n);i->input[1].data.resize(1024);i->input[2].data.resize(n);
 i->output[0].data.resize(n);i->output[1].data.resize(1024);return i;
}
void TfLiteInterpreterDelete(Interpreter* i){delete i;}
int TfLiteInterpreterAllocateTensors(Interpreter*){return 0;}
int TfLiteInterpreterInvoke(Interpreter* i){
 if(const auto* s=std::getenv("AEC_TEST_DELAY_MS"))std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(s)));
 if(!i->part)std::fill(i->output[0].data.begin(),i->output[0].data.end(),1);
 else for(int j=0;j<512;++j)i->output[0].data[j]=i->input[0].data[j]*.25;
 return 0;
}
Tensor* TfLiteInterpreterGetInputTensor(Interpreter* i,int n){return &i->input[n];}
Tensor* TfLiteInterpreterGetOutputTensor(Interpreter* i,int n){return &i->output[n];}
size_t TfLiteTensorByteSize(Tensor* t){return t->data.size()*4;}
int TfLiteTensorCopyFromBuffer(Tensor* t,const void* p,size_t n){std::memcpy(t->data.data(),p,n);return 0;}
int TfLiteTensorCopyToBuffer(Tensor* t,void* p,size_t n){std::memcpy(p,t->data.data(),n);return 0;}
}
