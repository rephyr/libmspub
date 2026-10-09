/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libmspub project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_LISTINFO_H
#define INCLUDED_LISTINFO_H

#include <boost/optional.hpp>

#include "NumberingDelimiter.h"
#include "NumberingType.h"

namespace libmspub
{
enum ListType
{
  ORDERED,
  UNORDERED
};
struct ListInfo
{
  ListType m_listType;

  //unordered list stuff
  boost::optional<unsigned> m_bulletChar;
  ListInfo(unsigned bulletChar) : m_listType(UNORDERED),
    m_bulletChar(bulletChar), m_numberIfRestarted(),
    m_numberingType(), m_numberingDelimiter(),
    m_formatCode(23), m_bulletCode(bulletChar), m_raw2(), m_delimiterRaw()
  {
  }

  //ordered list stuff
  boost::optional<unsigned> m_numberIfRestarted;
  boost::optional<NumberingType> m_numberingType;
  boost::optional<NumberingDelimiter> m_numberingDelimiter;

  // pub-converter: raw values as stored in the file (see OUR-PATCHES.md)
  unsigned m_formatCode;              // list sub-block 0x00: Word NFC code (0 decimal, 23 bullet, 255 none, ...)
  unsigned m_bulletCode;              // list sub-block 0x01: bullet character code
  boost::optional<unsigned> m_raw2;   // list sub-block 0x02: meaning unknown
  boost::optional<unsigned> m_delimiterRaw; // paragraph block 0x58
  ListInfo(boost::optional<unsigned> numberIfRestarted, NumberingType numberingType,
           NumberingDelimiter numberingDelimiter)
    : m_listType(ORDERED), m_bulletChar(),
      m_numberIfRestarted(numberIfRestarted), m_numberingType(numberingType),
      m_numberingDelimiter(numberingDelimiter),
      m_formatCode(unsigned(numberingType)), m_bulletCode(0), m_raw2(), m_delimiterRaw()
  {
  }
};
} // namespace libmspub

#endif /* INCLUDED_LISTINFO_H */
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
