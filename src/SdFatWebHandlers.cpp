/*
  Asynchronous WebServer library for Espressif MCUs

  Copyright (c) 2016 Hristo Gochkov. All rights reserved.
  This file is part of the esp8266 core for Arduino environment.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifdef ASYNCWEBSERVER_SDFAT_SUPPORT

#include "ESPAsyncWebServer.h"
#include "WebHandlerImpl.h"
#include "SdFatConfig.h"

//  _fs(sdfat), 

AsyncStaticSdFatWebHandler::AsyncStaticSdFatWebHandler(
    const char *uri, SdFat *sdfat, const char *path, const char *cache_control)
    : _fs(sdfat),_uri(uri),_path(path), _default_file("index.htm"),
      _cache_control(cache_control), _last_modified(""), _callback(nullptr) {
  // Ensure leading '/'
  if (_uri.length() == 0 || _uri[0] != '/')
    _uri = "/" + _uri;
  if (_path.length() == 0 || _path[0] != '/')
    _path = "/" + _path;

  // If path ends with '/' we assume a hint that this is a directory to improve
  // performance. However - if it does not end with '/' we, can't assume a file,
  // path can still be a directory.
  _isDir = _path[_path.length() - 1] == '/';

  // Remove the trailing '/' so we can handle default file
  // Notice that root will be "" not "/"
  if (_uri[_uri.length() - 1] == '/')
    _uri = _uri.substring(0, _uri.length() - 1);
  if (_path[_path.length() - 1] == '/')
    _path = _path.substring(0, _path.length() - 1);

  // Reset stats
  _gzipFirst = false;
  _gzipStats = 0xF8;
}

AsyncStaticSdFatWebHandler &AsyncStaticSdFatWebHandler::setIsDir(bool isDir) {
  _isDir = isDir;
  return *this;
}

AsyncStaticSdFatWebHandler &
AsyncStaticSdFatWebHandler::setDefaultFile(const char *filename) {
  _default_file = String(filename);
  return *this;
}

AsyncStaticSdFatWebHandler &
AsyncStaticSdFatWebHandler::setCacheControl(const char *cache_control) {
  _cache_control = String(cache_control);
  return *this;
}

AsyncStaticSdFatWebHandler &
AsyncStaticSdFatWebHandler::setLastModified(const char *last_modified) {
  _last_modified = String(last_modified);
  return *this;
}

AsyncStaticSdFatWebHandler &
AsyncStaticSdFatWebHandler::setLastModified(struct tm *last_modified) {
  char result[30];
  strftime(result, 30, "%a, %d %b %Y %H:%M:%S %Z", last_modified);
  return setLastModified((const char *)result);
}

#ifdef ESP8266
AsyncStaticSdFatWebHandler &
AsyncStaticSdFatWebHandler::setLastModified(time_t last_modified) {
  return setLastModified((struct tm *)gmtime(&last_modified));
}

AsyncStaticSdFatWebHandler &AsyncStaticSdFatWebHandler::setLastModified() {
  time_t last_modified;
  if (time(&last_modified) == 0) // time is not yet set
    return *this;
  return setLastModified(last_modified);
}
#endif
bool AsyncStaticSdFatWebHandler::canHandle(AsyncWebServerRequest *request) {
  DEBUGF("[AsyncStaticSdFatWebHandler::canHandle] url='%s'\n",
         request->url().c_str());

  if (request->method() != HTTP_GET || !request->url().startsWith(_uri) ||
      !request->isExpectedRequestedConnType(RCT_DEFAULT, RCT_HTTP)) {
    return false;
  }
  request->addInterestingHeader("Range");

  if (_getFile(request)) {
    // We interested in "If-Modified-Since" header to check if file was modified
    if (_last_modified.length())
      request->addInterestingHeader("If-Modified-Since");

    if (_cache_control.length())
      request->addInterestingHeader("If-None-Match");

    DEBUGF("[AsyncStaticSdFatWebHandler::canHandle] TRUE\n");
    return true;
  }

  return false;
}

bool AsyncStaticSdFatWebHandler::_getFile(AsyncWebServerRequest *request) {
  // Remove the found uri
  String path = request->url().substring(_uri.length());

  // We can skip the file check and look for default if request is to the root
  // of a directory or that request path ends with '/'
  bool canSkipFileCheck = (_isDir && path.length() == 0) ||
                          (path.length() && path[path.length() - 1] == '/');

  path = _path + path;

  // Do we have a file or .gz file
  if (!canSkipFileCheck && _fileExists(request, path))
    return true;

  // Can't handle if not default file
  if (_default_file.length() == 0)
    return false;

  // Try to add default file, ensure there is a trailing '/' ot the path.
  if (path.length() == 0 || path[path.length() - 1] != '/')
    path += "/";
  path += _default_file;

  return _fileExists(request, path);
}

#ifdef ESP32
#define FILE_IS_REAL(f) (f.isOpen() && !f.isDir())
#else
#define FILE_IS_REAL(f) (f == true)
#endif

bool AsyncStaticSdFatWebHandler::_fileExists(AsyncWebServerRequest *request,
                                             const String &path) {
  bool fileFound = false;
  bool gzipFound = false;

  String gzip = path + ".gz";

  if (_gzipFirst) {
    request->_sd_tempFile = _fs->open(gzip, O_RDONLY);
    gzipFound = FILE_IS_REAL(request->_sd_tempFile);
    if (!gzipFound) {
      request->_sd_tempFile = _fs->open(path, O_RDONLY);
      fileFound = FILE_IS_REAL(request->_sd_tempFile);
    }
  } else {
    request->_sd_tempFile = _fs->open(path, O_RDONLY);
    fileFound = FILE_IS_REAL(request->_sd_tempFile);
    if (!fileFound) {
      request->_sd_tempFile = _fs->open(gzip, O_RDONLY);
      gzipFound = FILE_IS_REAL(request->_sd_tempFile);
    }
  }

  bool found = fileFound || gzipFound;

  if (found) {
    // Extract the file name from the path and keep it in _tempObject
    size_t pathLen = path.length();
    char *_tempPath = (char *)malloc(pathLen + 1);
    snprintf(_tempPath, pathLen + 1, "%s", path.c_str());
    request->_tempObject = (void *)_tempPath;

    // Calculate gzip statistic
    _gzipStats = (_gzipStats << 1) + (gzipFound ? 1 : 0);
    if (_gzipStats == 0x00)
      _gzipFirst = false; // All files are not gzip
    else if (_gzipStats == 0xFF)
      _gzipFirst = true; // All files are gzip
    else
      _gzipFirst = _countBits(_gzipStats) >
                   4; // IF we have more gzip files - try gzip first
  }

  return found;
}

uint8_t AsyncStaticSdFatWebHandler::_countBits(const uint8_t value) const {
  uint8_t w = value;
  uint8_t n;
  for (n = 0; w != 0; n++)
    w &= w - 1;
  return n;
}

void AsyncStaticSdFatWebHandler::handleRequest(AsyncWebServerRequest *request) {

  // Get the filename from request->_tempObject and free it
  String filename = String((char *)request->_tempObject);
  free(request->_tempObject);
  request->_tempObject = NULL;
  if ((_username != "" && _password != "") &&
      !request->authenticate(_username.c_str(), _password.c_str()))
    return request->requestAuthentication();

  if (request->hasHeader("Range")) {

    String rh = request->header("Range");
    if (rh.startsWith("bytes=")) {
      int dash = rh.indexOf('-');
      String tmp = rh.substring(6, dash);
      char *ptr;
      uint64_t start = strtoull(tmp.c_str(), &ptr, 10);
      tmp = rh.substring(dash + 1, rh.length());
      uint64_t end = strtoull(tmp.c_str(), &ptr, 10);

      DEBUGF("SdFat Range start=%llu length=%llu\n", start, end - start);
      AsyncWebServerResponse *response = new AsyncSdFatFileResponse(
          request->_sd_tempFile, filename, String(), false, _callback, start, end);
      request->send(response);
    }
  } else if (request->_sd_tempFile == true) {
    String etag = String(request->_sd_tempFile.fileSize());
    if (_last_modified.length() &&
        _last_modified == request->header("If-Modified-Since")) {
      request->_sd_tempFile.close();
      request->send(304); // Not modified
    } else if (_cache_control.length() && request->hasHeader("If-None-Match") &&
               request->header("If-None-Match").equals(etag)) {
      request->_sd_tempFile.close();
      AsyncWebServerResponse *response =
          new AsyncBasicResponse(304); // Not modified
      response->addHeader("Cache-Control", _cache_control);
      response->addHeader("ETag", etag);
      request->send(response);
    } else {
      AsyncWebServerResponse *response = new AsyncSdFatFileResponse(
          request->_sd_tempFile, filename, String(), false, _callback);
      if (_last_modified.length())
        response->addHeader("Last-Modified", _last_modified);
      if (_cache_control.length()) {
        response->addHeader("Cache-Control", _cache_control);
        response->addHeader("ETag", etag);
      }
      request->send(response);
    }
  } else {
    request->send(404);
  }
}

// SdFat AsyncStaticSdFatWebHandler::_fs;

// bool AsyncStaticSdFatWebHandler::begin(SdSpiConfig spiConfig) {
//   return _fs.begin(spiConfig);
// }


#endif