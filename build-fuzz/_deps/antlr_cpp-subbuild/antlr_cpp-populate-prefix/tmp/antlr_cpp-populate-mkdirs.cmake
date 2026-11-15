# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-src"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-build"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-subbuild/antlr_cpp-populate-prefix"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-subbuild/antlr_cpp-populate-prefix/tmp"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-subbuild/antlr_cpp-populate-prefix/src/antlr_cpp-populate-stamp"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-subbuild/antlr_cpp-populate-prefix/src"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-subbuild/antlr_cpp-populate-prefix/src/antlr_cpp-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-subbuild/antlr_cpp-populate-prefix/src/antlr_cpp-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzz/_deps/antlr_cpp-subbuild/antlr_cpp-populate-prefix/src/antlr_cpp-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
