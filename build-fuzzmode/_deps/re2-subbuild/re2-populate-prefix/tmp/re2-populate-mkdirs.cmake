# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-src"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-build"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-subbuild/re2-populate-prefix"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-subbuild/re2-populate-prefix/tmp"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-subbuild/re2-populate-prefix/src/re2-populate-stamp"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-subbuild/re2-populate-prefix/src"
  "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-subbuild/re2-populate-prefix/src/re2-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-subbuild/re2-populate-prefix/src/re2-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/srv/fb2/fangzhengmei/0064-libwebp-8-d6999d5b/build-fuzzmode/_deps/re2-subbuild/re2-populate-prefix/src/re2-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
