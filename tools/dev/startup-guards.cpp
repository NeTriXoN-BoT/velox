#include "phone.cpp"
#include <iostream>
#include <stdexcept>
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main(){try{
 using namespace phone;std::wstring root=exeFolder()+L"guard-fixtures";createDirs(root);
 SetEnvironmentVariableW(L"ANDROID_USER_HOME",(root+L"\\user").c_str());SetEnvironmentVariableW(L"ANDROID_EMULATOR_HOME",(root+L"\\emulator").c_str());std::wstring error;
 check(prepareAndroidConfig(error)&&isDir(root+L"\\user")&&isDir(root+L"\\emulator"),"Missing configuration folders were not prepared");
 std::wstring blocked=root+L"\\not-a-folder.txt";{std::ofstream f{std::filesystem::path(blocked)};f<<"test fixture";}
 SetEnvironmentVariableW(L"ANDROID_USER_HOME",blocked.c_str());check(!prepareAndroidConfig(error)&&!error.empty(),"An invalid configuration folder was accepted");
 g_paths.log=root+L"\\oversized-log.txt";{std::ofstream f{std::filesystem::path(g_paths.log),std::ios::binary};f.seekp(17*1024*1024);f<<"Saving snapshot 'default_boot' took 100 ms\nERROR: bounded tail sentinel\n";}
 auto log=readLog();check(log.size()<=(8u<<20)&&log.find("bounded tail sentinel")!=std::string::npos,"Diagnostic reads aren't bounded");check(!logSavedSnapshot(),"Oversized log incorrectly trusted a snapshot");
 g_hostReady=true;g_host.threads=4;g_host.ramGB=8;Settings s;s.cores=32;s.memory=32;
 check(!coreAllowed(8)&&memoryAllowed(4)&&!memoryAllowed(8)&&coresFor(s)<=4&&memoryFor(s)<=5,"Low-end host protection failed");
 g_host.threads=32;g_host.ramGB=48;check(coreAllowed(32)&&memoryAllowed(32)&&coresFor(s)==32&&memoryFor(s)==32,"High-end allocation limits failed");
 check(!coreAllowed(-1)&&!coreAllowed(64)&&!memoryAllowed(-1)&&!memoryAllowed(64),"Unsupported allocation accepted");
 g_host.threads=128;g_host.ramGB=128;s.cores=64;s.memory=64;check(coreAllowed(64)&&memoryAllowed(64)&&coresFor(s)==64&&memoryFor(s)==64,"64-unit host allocation failed");
 check(!coreAllowed(96)&&!memoryAllowed(96),"Beyond64 maximum accepted");
 std::cout<<"PASS: missing/invalid configuration directories, bounded diagnostic reads, snapshot distrust and host resource limits\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
