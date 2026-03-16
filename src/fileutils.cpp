// Caprice 32
// File IO functions

#ifndef _MSC_VER
#include <dirent.h>
#include <unistd.h>
#endif
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <stdio.h>
#include <sys/stat.h>
#include <ctime>

int file_size (int fd) {
   struct stat s;

   if (!fstat(fd, &s)) {
      return s.st_size;
   }
   return 0;
}

bool file_copy(FILE *in, FILE *out) {
  size_t read;
  char buffer[1024];
  while((read = fread(buffer, 1, 1024, in)) > 0) {
      if (fwrite(buffer, 1, read, out) != read) {
        break;
      }
  }

  return !(ferror(in) || ferror(out));
}

bool is_directory(std::string filepath) {
  struct stat _stat;

  return ( (stat(filepath.c_str(), &_stat) == 0) && (S_ISDIR(_stat.st_mode)) );
}

// Returns a vector containing the names of the files in the specified directory
std::vector<std::string> listDirectory(std::string &directory) {
   std::vector<std::string> s;

   if (directory[directory.size() - 1] != '/') {
      directory += "/";
   }
#ifdef _MSC_VER
   // MSVC: use std::filesystem instead of POSIX dirent
   std::error_code ec;
   for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
       std::string fileName = entry.path().filename().string();
       if (fileName != ".." && fileName != ".") {
           s.push_back(fileName);
       }
   }
   if (ec) {
       printf("directory_iterator(%s) failed; %s\n", directory.c_str(), ec.message().c_str());
       return s;
   }
#else
   DIR* pDir;
   struct dirent *pent;
   pDir = opendir(directory.c_str());
   if (!pDir){
       printf ("opendir(%s) failed; terminating\n", directory.c_str());
       return s;
   }
   while ((pent = readdir(pDir))){
       std::string fileName = std::string(pent->d_name);
       if (fileName != ".." and fileName != ".") {
           s.push_back(fileName);
       }
   }
   closedir(pDir);
#endif
   sort(s.begin(), s.end()); // sort elements
   return s;
}

// Returns a vector containing the names of the files having extension "ext" in
// the specified directory
std::vector<std::string> listDirectoryExt(std::string &directory, const std::string &ext) {
  std::vector<std::string> allFiles = listDirectory(directory);
  std::vector<std::string> matchingFiles;
  std::string extension;

  for (const auto& fileName : allFiles) {
    extension = fileName.substr(fileName.find_last_of('.') + 1);
    if (ext == extension) {
      matchingFiles.push_back(fileName);
    }
  }
  return matchingFiles;
}

#define TIME_STRING_MAX_LENGTH 80
std::string getDateString() {
  char dateString[TIME_STRING_MAX_LENGTH]; // Should be more than enough
  time_t t = std::time(nullptr);
  if (std::strftime(dateString, sizeof(dateString), "%Y%m%d_%H%M%S", std::localtime(&t))) {
    return std::string(dateString);
  }
  return "unknown_date";
}
