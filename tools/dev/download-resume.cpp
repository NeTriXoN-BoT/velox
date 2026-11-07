#include "net.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <stdexcept>
void check(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
int main(){try{
 std::wstring folder=phone::exeFolder()+L"download-resume";phone::createDirs(folder);std::wstring file=folder+L"\\platform-tools.zip",error;std::atomic<bool> cancel{false};
 const std::wstring url=L"https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip";const std::string sha="e03e78b1d80b396f1c3358e31251cb31740e1110";constexpr uint64_t size=8044989;
 bool ok=phone::net::download(url,file,size,sha,phone::net::Hash::Sha1,[&](uint64_t done,uint64_t){if(done>1024*1024)cancel=true;},&cancel,error);
 check(!ok&&cancel&&error==L"cancelled","Download didn't cancel");auto partial=std::filesystem::file_size(file+L".part");check(partial>0&&partial<size,"Partial download not retained");
 cancel=false;error.clear();check(phone::net::download(url,file,size,sha,phone::net::Hash::Sha1,{},&cancel,error),"Resumed download failed");check(phone::net::hashFile(file,phone::net::Hash::Sha1)==sha&&!phone::exists(file+L".part"),"Resumed file was not verified");
 {std::fstream f(std::filesystem::path(file),std::ios::binary|std::ios::in|std::ios::out);f.put('X');}
 check(phone::net::download(url,file,size,sha,phone::net::Hash::Sha1,{},&cancel,error)&&phone::net::hashFile(file,phone::net::Hash::Sha1)==sha,"Damaged cached package was trusted");
 std::cout<<"PASS: real Google download cancellation, retained partial, retry/resume, SHA verification and damaged-cache replacement\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
