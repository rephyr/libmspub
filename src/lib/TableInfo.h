/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libmspub project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef INCLUDED_TABLEINFO_H
#define INCLUDED_TABLEINFO_H

#include <vector>
#include <boost/optional.hpp>

namespace libmspub
{

struct CellInfo
{
  CellInfo()
    : m_startRow()
    , m_endRow()
    , m_startColumn()
    , m_endColumn()
    , m_marginsEmu()
  {
  }

  unsigned m_startRow;
  unsigned m_endRow;
  unsigned m_startColumn;
  unsigned m_endColumn;
  // pub-converter: CELLS record blocks 0x0a-0x0d, [guess] left, right, top, bottom text margins (EMU)
  boost::optional<unsigned> m_marginsEmu[4];
};

// pub-converter: one table border segment, drawn by Publisher as a separate Escher shape whose
// ClientAnchor is a block list (0x01 = 1 horizontal / 2 vertical, 0x02 = table seqnum,
// 0x04/0x05 = start row/column grid line, 0x06/0x07 = end row/column grid line). Reverse-engineered.
struct TableBorderInfo
{
  unsigned m_kind;
  unsigned m_row1, m_col1, m_row2, m_col2;
  unsigned m_color;      // OfficeArtCOLORREF of the shape fill (the border colour)
  bool m_visible;        // shape fill on (fFilled)
  unsigned m_widthEmu;   // lineWidth (default 9525)
  boost::optional<unsigned> m_dashing; // lineDashing, raw
  TableBorderInfo() : m_kind(0), m_row1(0), m_col1(0), m_row2(0), m_col2(0), m_color(0), m_visible(true),
    m_widthEmu(9525), m_dashing() { }
};

// pub-converter: one cell fill, an Escher shape whose ClientAnchor has no 0x01 and 0x03 = cell index
// (the cell's position in the CELLS record list).
struct TableCellFillInfo
{
  unsigned m_cellIndex;
  unsigned m_color;
  bool m_filled;
  boost::optional<unsigned> m_opacity; // fillOpacity, 16.16
  TableCellFillInfo() : m_cellIndex(0), m_color(0), m_filled(true), m_opacity() { }
};

struct TableInfo
{
  std::vector<unsigned> m_rowHeightsInEmu;
  std::vector<unsigned> m_columnWidthsInEmu;
  unsigned m_numRows;
  unsigned m_numColumns;
  std::vector<CellInfo> m_cells;
  TableInfo(unsigned numRows, unsigned numColumns) : m_rowHeightsInEmu(),
    m_columnWidthsInEmu(), m_numRows(numRows), m_numColumns(numColumns),
    m_cells()
  {
  }
};
} // namespace libmspub

#endif /* INCLUDED_TABLEINFO_H */
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
