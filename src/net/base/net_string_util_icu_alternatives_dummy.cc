// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/net_string_util.h"

#include "base/logging.h"
#include "base/strings/string_piece.h"

namespace net {

const char* const kCharsetLatin1 = "";

bool ConvertToUtf8(base::StringPiece text, const char* charset,
                   std::string* output) {
  NOTIMPLEMENTED();
  return false;
}

bool ConvertToUtf8AndNormalize(base::StringPiece text, const char* charset,
                               std::string* output) {
  NOTIMPLEMENTED();
  return false;
}

bool ConvertToUTF16(base::StringPiece text, const char* charset,
                    base::string16* output) {
  NOTIMPLEMENTED();
  return false;
}

bool ConvertToUTF16WithSubstitutions(base::StringPiece text,
                                     const char* charset,
                                     base::string16* output) {
  NOTIMPLEMENTED();
  return false;
}

bool ToUpper(base::StringPiece16 str, base::string16* output) {
  NOTIMPLEMENTED();
  return false;
}

}  // namespace net
