/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libmspub project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "MSPUBCollector.h"

#include <algorithm>
#include <functional>
#include <math.h>
#include <memory>

#include <boost/multi_array.hpp>

#include <unicode/ucsdet.h>
#include <unicode/uloc.h>

#include "Arrow.h"
#include "Coordinate.h"
#include "Dash.h"
#include "Fill.h"
#include "Line.h"
#include "Margins.h"
#include "MSPUBConstants.h"
#include "MSPUBTypes.h"
#include "PolygonUtils.h"
#include "Shadow.h"
#include "ShapeGroupElement.h"
#include "TableInfo.h"
#include "VectorTransformation2D.h"
#include "libmspub_utils.h"

namespace libmspub
{

using namespace std::placeholders;

namespace
{

static void separateTabsAndInsertText(librevenge::RVNGDrawingInterface *iface, const librevenge::RVNGString &text)
{
  if (!iface || text.empty())
    return;
  librevenge::RVNGString tmpText;
  librevenge::RVNGString::Iter i(text);
  for (i.rewind(); i.next();)
  {
    if (*(i()) == '\t')
    {
      if (!tmpText.empty())
      {
        if (iface)
          iface->insertText(tmpText);
        tmpText.clear();
      }

      if (iface)
        iface->insertTab();
    }
    else if (*(i()) == '\n')
    {
      if (!tmpText.empty())
      {
        if (iface)
          iface->insertText(tmpText);
        tmpText.clear();
      }

      if (iface)
        iface->insertLineBreak();
    }
    else
    {
      tmpText.append(i());
    }
  }
  if (iface && !tmpText.empty())
    iface->insertText(tmpText);
}

static void separateSpacesAndInsertText(librevenge::RVNGDrawingInterface *iface, const librevenge::RVNGString &text)
{
  if (!iface)
    return;
  if (text.empty())
  {
    iface->insertText(text);
    return;
  }
  librevenge::RVNGString tmpText;
  int numConsecutiveSpaces = 0;
  librevenge::RVNGString::Iter i(text);
  for (i.rewind(); i.next();)
  {
    if (*(i()) == ' ')
      numConsecutiveSpaces++;
    else
      numConsecutiveSpaces = 0;

    if (numConsecutiveSpaces > 1)
    {
      if (!tmpText.empty())
      {
        separateTabsAndInsertText(iface, tmpText);
        tmpText.clear();
      }

      if (iface)
        iface->insertSpace();
    }
    else
    {
      tmpText.append(i());
    }
  }
  separateTabsAndInsertText(iface, tmpText);
}

// pub-converter: U+FDD0 (UTF-8 EF B7 90) marks a page-number field (see MSPUBParser::parseQuill)
static void insertTextWithFields(librevenge::RVNGDrawingInterface *iface, const librevenge::RVNGString &text)
{
  static const char marker[] = "\xef\xb7\x90";
  const std::string all(text.cstr());
  if (!iface || all.find(marker) == std::string::npos)
  {
    separateSpacesAndInsertText(iface, text);
    return;
  }
  size_t from = 0;
  for (;;)
  {
    const size_t at = all.find(marker, from);
    const std::string part = all.substr(from, at == std::string::npos ? std::string::npos : at - from);
    if (!part.empty())
      separateSpacesAndInsertText(iface, librevenge::RVNGString(part.c_str()));
    if (at == std::string::npos)
      break;
    librevenge::RVNGPropertyList field;
    field.insert("librevenge:field-type", "text:page-number");
    field.insert("style:num-format", "1");
    iface->insertField(field);
    from = at + 3;
  }
}

struct TableLayoutCell
{
  TableLayoutCell()
    : m_cell(0)
    , m_rowSpan(0)
    , m_colSpan(0)
  {
  }

  unsigned m_cell;
  unsigned m_rowSpan;
  unsigned m_colSpan;
};

bool isCovered(const TableLayoutCell &cell)
{
  assert((cell.m_rowSpan == 0) == (cell.m_colSpan == 0));
  return (cell.m_rowSpan == 0) && (cell.m_colSpan == 0);
}

typedef boost::multi_array<TableLayoutCell, 2> TableLayout;

void createTableLayout(const std::vector<CellInfo> &cells, TableLayout &tableLayout)
{
  for (auto it = cells.begin(); it != cells.end(); ++it)
  {
    if ((it->m_endRow >= tableLayout.shape()[0]) || (it->m_endColumn >= tableLayout.shape()[1]))
    {
      MSPUB_DEBUG_MSG((
                        "cell %u (rows %u to %u, columns %u to %u) overflows the table, ignoring\n",
                        unsigned(int(it - cells.begin())),
                        it->m_startRow, it->m_endRow,
                        it->m_startColumn, it->m_endColumn
                      ));
      continue;
    }
    if (it->m_startRow > it->m_endRow)
    {
      MSPUB_DEBUG_MSG((
                        "cell %u (rows %u to %u) has got negative row span, ignoring\n",
                        unsigned(int(it - cells.begin())),
                        it->m_startRow, it->m_endRow
                      ));
      continue;
    }
    if (it->m_startColumn > it->m_endColumn)
    {
      MSPUB_DEBUG_MSG((
                        "cell %u (columns %u to %u) has got negative column span, ignoring\n",
                        unsigned(int(it - cells.begin())),
                        it->m_startColumn, it->m_endColumn
                      ));
      continue;
    }

    const unsigned rowSpan = it->m_endRow - it->m_startRow + 1;
    const unsigned colSpan = it->m_endColumn - it->m_startColumn + 1;

    if ((rowSpan == 0) != (colSpan == 0))
    {
      MSPUB_DEBUG_MSG((
                        "cell %u (rows %u to %u, columns %u to %u) has got 0 span in one dimension, ignoring\n",
                        unsigned(int(it - cells.begin())),
                        it->m_startRow, it->m_endRow,
                        it->m_startColumn, it->m_endColumn
                      ));
      continue;
    }

    TableLayoutCell &layoutCell = tableLayout[it->m_startRow][it->m_startColumn];
    layoutCell.m_cell = unsigned(int(it - cells.begin()));
    layoutCell.m_rowSpan = rowSpan;
    layoutCell.m_colSpan = colSpan;
  }
}

typedef std::vector<std::pair<unsigned, unsigned> > ParagraphToCellMap_t;
typedef std::vector<librevenge::RVNGString> SpanTexts_t;
typedef std::vector<SpanTexts_t> ParagraphTexts_t;

void mapTableTextToCells(
  const std::vector<TextParagraph> &text,
  const std::vector<unsigned> &tableCellTextEnds,
  const char *const encoding,
  ParagraphToCellMap_t &paraToCellMap,
  ParagraphTexts_t &paraTexts
)
{
  assert(paraToCellMap.empty());
  assert(paraTexts.empty());

  paraToCellMap.reserve(tableCellTextEnds.size());
  paraTexts.reserve(tableCellTextEnds.size());

  unsigned firstPara = 0;
  unsigned offset = 1;
  for (unsigned para = 0; para != text.size() && paraToCellMap.size() < tableCellTextEnds.size(); ++para)
  {
    paraTexts.push_back(SpanTexts_t());
    paraTexts.back().reserve(text[para].spans.size());

    for (unsigned i_spans = 0; i_spans != text[para].spans.size(); ++i_spans)
    {
      librevenge::RVNGString textString;
      appendCharacters(textString, text[para].spans[i_spans].chars, encoding);
      offset += textString.len();
      // TODO: why do we not drop these during parse already?
      if ((i_spans == text[para].spans.size() - 1) && (textString == "\r"))
        continue;
      paraTexts.back().push_back(textString);
    }

    assert(paraTexts.back().size() <= text[para].spans.size());

    if (offset >= tableCellTextEnds[paraToCellMap.size()])
    {
      if (offset > tableCellTextEnds[paraToCellMap.size()])
      {
        MSPUB_DEBUG_MSG(("text of cell %u ends in the middle of a paragraph!\n", unsigned(paraToCellMap.size())));
      }

      paraToCellMap.push_back(std::make_pair(firstPara, para));
      firstPara = para + 1;
    }
  }

  assert(paraTexts.size() == text.size());
  assert(paraToCellMap.size() <= tableCellTextEnds.size());
}

void fillUnderline(librevenge::RVNGPropertyList &props, const Underline underline)
{
  switch (underline)
  {
  case Underline::None:
    return;
  case Underline::Single:
  case Underline::WordsOnly:
  case Underline::Double:
  case Underline::Thick:
    props.insert("style:text-underline-style", "solid");
    break;
  case Underline::Dotted:
  case Underline::ThickDot:
    props.insert("style:text-underline-style", "dotted");
    break;
  case Underline::Dash:
  case Underline::ThickDash:
    props.insert("style:text-underline-style", "dash");
    break;
  case Underline::DotDash:
  case Underline::ThickDotDash:
    props.insert("style:text-underline-style", "dot-dash");
    break;
  case Underline::DotDotDash:
  case Underline::ThickDotDotDash:
    props.insert("style:text-underline-style", "dot-dot-dash");
    break;
  case Underline::Wave:
  case Underline::ThickWave:
  case Underline::DoubleWave:
    props.insert("style:text-underline-style", "wave");
    break;
  case Underline::LongDash:
  case Underline::ThickLongDash:
    props.insert("style:text-underline-style", "long-dash");
    break;
  }

  switch (underline)
  {
  case Underline::Double:
  case Underline::DoubleWave:
    props.insert("style:text-underline-type", "double");
    break;
  default:
    props.insert("style:text-underline-type", "single");
    break;
  }

  switch (underline)
  {
  case Underline::Thick:
  case Underline::ThickWave:
  case Underline::ThickDot:
  case Underline::ThickDash:
  case Underline::ThickDotDash:
  case Underline::ThickDotDotDash:
    props.insert("style:text-underline-width", "bold");
    break;
  default:
    props.insert("style:text-underline-width", "auto");
    break;
  }

  switch (underline)
  {
  case Underline::WordsOnly:
    props.insert("style:text-underline-mode", "skip-white-space");
    break;
  default:
    props.insert("style:text-underline-mode", "continuous");
    break;
  }
}

void fillLocale(librevenge::RVNGPropertyList &props, const unsigned lcid)
{
  char locale[ULOC_FULLNAME_CAPACITY];
  UErrorCode status = U_ZERO_ERROR;
  uloc_getLocaleForLCID(lcid, locale, ULOC_FULLNAME_CAPACITY, &status);
  if (!U_SUCCESS(status))
    return;
  char component[ULOC_FULLNAME_CAPACITY];
  int32_t len = uloc_getLanguage(locale, component, ULOC_FULLNAME_CAPACITY, &status);
  if (U_SUCCESS(status) && len > 0)
    props.insert("fo:language", component);
  len = uloc_getCountry(locale, component, ULOC_FULLNAME_CAPACITY, &status);
  if (U_SUCCESS(status) && len > 0)
    props.insert("fo:country", component);
  len = uloc_getScript(locale, component, ULOC_FULLNAME_CAPACITY, &status);
  if (U_SUCCESS(status) && len > 0)
    props.insert("fo:script", component);
}

} // anonymous namespace

void MSPUBCollector::collectMetaData(const librevenge::RVNGPropertyList &metaData)
{
  m_metaData = metaData;
}

void MSPUBCollector::addEOTFont(const librevenge::RVNGString &name, const librevenge::RVNGBinaryData &data)
{
  m_embeddedFonts.push_back(EmbeddedFontInfo(name, data));
}

void MSPUBCollector::setShapePictureRecolor(unsigned seqNum,
                                            const ColorReference &recolor)
{
  m_shapeInfosBySeqNum[seqNum].m_pictureRecolor = recolor;
}

void MSPUBCollector::setShapePictureBrightness(unsigned seqNum,
                                               int brightness)
{
  m_shapeInfosBySeqNum[seqNum].m_pictureBrightness = brightness;
}

void MSPUBCollector::setShapePictureContrast(unsigned seqNum,
                                             int contrast)
{
  m_shapeInfosBySeqNum[seqNum].m_pictureContrast = contrast;
}

void MSPUBCollector::setShapePictureCrop(unsigned seqNum, double top, double bottom, double left, double right)
{
  std::vector<double> crop;
  crop.push_back(top);
  crop.push_back(bottom);
  crop.push_back(left);
  crop.push_back(right);
  m_shapeInfosBySeqNum[seqNum].m_pictureCrop = crop;
}

// pub-converter patch 13: table border segments and cell fills (Escher shapes anchored to a table)
void MSPUBCollector::addTableBorder(unsigned tableSeqNum, const TableBorderInfo &border)
{
  std::vector<TableBorderInfo> &v = m_tableBordersBySeqNum[tableSeqNum];
  if (v.size() < 100000)
    v.push_back(border);
}

void MSPUBCollector::addTableCellFill(unsigned tableSeqNum, const TableCellFillInfo &fill)
{
  std::vector<TableCellFillInfo> &v = m_tableCellFillsBySeqNum[tableSeqNum];
  if (v.size() < 100000)
    v.push_back(fill);
}

void MSPUBCollector::setShapeWrapDistance(unsigned seqNum, unsigned side, unsigned emu)
{
  if (side < 4)
    m_shapeInfosBySeqNum[seqNum].m_wrapDistances[side] = emu;
}

void MSPUBCollector::setShapeWrapShapes(unsigned seqNum, const std::vector<unsigned> &shapes)
{
  m_shapeInfosBySeqNum[seqNum].m_wrapShapes = shapes;
}

void MSPUBCollector::setShapeWordArt(unsigned seqNum, const WordArtInfo &wordArt)
{
  m_shapeInfosBySeqNum[seqNum].m_wordArt = wordArt;
}

namespace
{
// GeometryTextBooleanProperties: a value bit counts only if its "use" bit (value bit + 16) is set
bool gtextFlag(unsigned props, unsigned bit)
{
  return (props & (1u << bit)) && (props & (1u << (bit + 16)));
}
}

// pub-converter: draw WordArt as a text object (the text, font, size, bold/italic,
// fill colour) instead of filling the warp outline, which came out as a black blob.
// The warp itself (pub:wordart-shape = Escher shape type) is left to the consumer.
void MSPUBCollector::paintWordArt(const ShapeInfo &info, const librevenge::RVNGPropertyList &fillProps, const VectorTransformation2D &thisTransform) const
{
  const WordArtInfo &wa = get(info.m_wordArt);
  librevenge::RVNGPropertyList style(fillProps);
  bool hasLine = false;
  for (const auto &line : info.m_lines)
  {
    if (line.m_lineExists)
    {
      style.insert("draw:stroke", "solid");
      style.insert("svg:stroke-color", getColorString(line.m_color.getFinalColor(m_paletteColors)));
      style.insert("svg:stroke-width", double(line.m_widthInEmu) / EMUS_IN_INCH);
      hasLine = true;
      break;
    }
  }
  if (!hasLine)
    style.insert("draw:stroke", "none");
  m_painter->setStyle(style);

  librevenge::RVNGPropertyList props;
  setRectCoordProps(info.m_coordinates.get_value_or(Coordinate()), &props);
  const double rotation = thisTransform.getRotation();
  if (rotation != 0)
    props.insert("librevenge:rotate", rotation * 180 / M_PI);
  props.insert("fo:padding-left", 0.);
  props.insert("fo:padding-top", 0.);
  props.insert("fo:padding-right", 0.);
  props.insert("fo:padding-bottom", 0.);
  props.insert("draw:textarea-vertical-align", "middle");
  props.insert("pub:wordart", true);
  props.insert("pub:wordart-shape", int(info.m_type.get_value_or(0)));
  const unsigned bp = wa.m_boolProps;
  const char *fit = gtextFlag(bp, 10) ? "stretch" : gtextFlag(bp, 8) ? "best-fit" : gtextFlag(bp, 9) ? "shrink" : "none";
  props.insert("pub:wordart-fit", fit);
  if (gtextFlag(bp, 13))
    props.insert("pub:wordart-vertical", true);
  if (gtextFlag(bp, 12))
    props.insert("pub:wordart-kern", true);
  if (bool(wa.m_spacing))
    props.insert("pub:wordart-spacing", get(wa.m_spacing));
  // pub-converter patch 13: warp adjust values (raw MS-ODRAW adjustValue..adjust10Value) and flips
  if (!wa.m_adjust.empty())
  {
    librevenge::RVNGPropertyListVector adj;
    for (const auto &a : wa.m_adjust)
    {
      librevenge::RVNGPropertyList item;
      item.insert("pub:index", int(a.first));
      item.insert("pub:value", a.second);
      adj.append(item);
    }
    props.insert("pub:wordart-adjust", adj);
  }
  if (bool(info.m_flips))
  {
    if (get(info.m_flips).second)
      props.insert("pub:flip-h", true);
    if (get(info.m_flips).first)
      props.insert("pub:flip-v", true);
  }
  if (bool(info.m_seqNum))
    props.insert("pub:shape-id", int(get(info.m_seqNum)));
  m_painter->startTextObject(props);

  librevenge::RVNGPropertyList paraProps;
  switch (wa.m_align.get_value_or(1))
  {
  case 2:
    paraProps.insert("fo:text-align", "left");
    break;
  case 3:
    paraProps.insert("fo:text-align", "right");
    break;
  case 4:
  case 5:
    paraProps.insert("fo:text-align", "justify");
    break;
  default: // 1 = center, 0 = stretch (centred, see pub:wordart-fit)
    paraProps.insert("fo:text-align", "center");
    break;
  }
  librevenge::RVNGPropertyList spanProps;
  if (!wa.m_font.empty())
  {
    librevenge::RVNGString font;
    appendCharacters(font, wa.m_font, "UTF-16LE");
    spanProps.insert("style:font-name", font);
  }
  // inches, like every other libmspub font size; Publisher's default is 36 pt
  spanProps.insert("fo:font-size", wa.m_sizePt.get_value_or(36.) / POINTS_IN_INCH);
  if (gtextFlag(bp, 5))
    spanProps.insert("fo:font-weight", "bold");
  if (gtextFlag(bp, 4))
    spanProps.insert("fo:font-style", "italic");
  if (gtextFlag(bp, 3))
    spanProps.insert("style:text-underline-type", "single");
  if (gtextFlag(bp, 1))
    spanProps.insert("fo:font-variant", "small-caps");
  if (gtextFlag(bp, 0))
    spanProps.insert("style:text-line-through-type", "single");
  if (gtextFlag(bp, 2))
    spanProps.insert("fo:text-shadow", "1pt 1pt");
  if (fillProps["draw:fill"] && fillProps["draw:fill"]->getStr() == "solid" && fillProps["draw:fill-color"])
    spanProps.insert("fo:color", fillProps["draw:fill-color"]->getStr());
  else
    spanProps.insert("fo:color", "#000000");

  // one paragraph per line; lines are separated by CR and/or LF, NUL ends the text
  std::vector<std::vector<unsigned char> > lines(1);
  for (size_t i = 0; i + 1 < wa.m_text.size(); i += 2)
  {
    const unsigned ch = wa.m_text[i] | (wa.m_text[i + 1] << 8);
    if (ch == 0)
      break;
    if (ch == '\r' || ch == '\n')
    {
      if (ch == '\n' && i >= 2 && (wa.m_text[i - 2] | (wa.m_text[i - 1] << 8)) == '\r')
        continue;
      lines.push_back(std::vector<unsigned char>());
      continue;
    }
    lines.back().push_back(wa.m_text[i]);
    lines.back().push_back(wa.m_text[i + 1]);
  }
  for (const auto &line : lines)
  {
    m_painter->openParagraph(paraProps);
    m_painter->openSpan(spanProps);
    if (!line.empty())
    {
      librevenge::RVNGString text;
      appendCharacters(text, line, "UTF-16LE");
      separateSpacesAndInsertText(m_painter, text);
    }
    m_painter->closeSpan();
    m_painter->closeParagraph();
  }
  m_painter->endTextObject();
}

void MSPUBCollector::setShapeBeginArrow(unsigned seqNum,
                                        const Arrow &arrow)
{
  m_shapeInfosBySeqNum[seqNum].m_beginArrow = arrow;
}

void MSPUBCollector::setShapeVerticalTextAlign(unsigned seqNum,
                                               VerticalAlign va)
{
  m_shapeInfosBySeqNum[seqNum].m_verticalAlign = va;
}

void MSPUBCollector::setShapeEndArrow(unsigned seqNum,
                                      const Arrow &arrow)
{
  m_shapeInfosBySeqNum[seqNum].m_endArrow = arrow;
}

void MSPUBCollector::setShapeTableInfo(unsigned seqNum,
                                       const TableInfo &ti)
{
  m_shapeInfosBySeqNum[seqNum].m_tableInfo = ti;
}

void MSPUBCollector::setShapeNumColumns(unsigned seqNum,
                                        unsigned numColumns)
{
  m_shapeInfosBySeqNum[seqNum].m_numColumns = numColumns;
}

void MSPUBCollector::setShapeColumnSpacing(unsigned seqNum,
                                           unsigned spacing)
{
  m_shapeInfosBySeqNum[seqNum].m_columnSpacing = spacing;
}

void MSPUBCollector::setShapeStretchBorderArt(unsigned seqNum)
{
  m_shapeInfosBySeqNum[seqNum].m_stretchBorderArt = true;
}

void MSPUBCollector::setRectCoordProps(Coordinate coord, librevenge::RVNGPropertyList *props) const
{
  props->insert("svg:x", coord.getXIn(m_width));
  props->insert("svg:y", coord.getYIn(m_height));
  props->insert("svg:width", coord.getWidthIn());
  props->insert("svg:height", coord.getHeightIn());
}

Coordinate getFudgedCoordinates(Coordinate coord, const std::vector<Line> &lines, bool makeBigger, BorderPosition borderPosition)
{
  Coordinate fudged = coord;
  unsigned topFudge = 0;
  unsigned rightFudge = 0;
  unsigned bottomFudge = 0;
  unsigned leftFudge = 0;
  // pub-converter patch 14: ONE line means the same line on all four sides. Upstream fudged only the top edge
  // (lines[0]), so a thick frame's top sat half a line width too high (osp-*: 20 pt frame, top 0.14 in high in
  // every page vs the Publisher PDF; left, right and bottom were right). With a half-inside border the line is
  // drawn centred on the shape's edge, as in that PDF: no change to the stroke rectangle; the text area shrinks by
  // half the width on every side.
  const bool oneLine = lines.size() == 1;
  if (oneLine && borderPosition == HALF_INSIDE_SHAPE && makeBigger)
    return fudged;
  switch (borderPosition)
  {
  case HALF_INSIDE_SHAPE:
    topFudge = (!lines.empty()) ? lines[0].m_widthInEmu / 2 : 0;
    rightFudge = (lines.size() > 1) ? lines[1].m_widthInEmu / 2 : oneLine ? topFudge : 0;
    bottomFudge = (lines.size() > 2) ? lines[2].m_widthInEmu / 2 : oneLine ? topFudge : 0;
    leftFudge = (lines.size() > 3) ? lines[3].m_widthInEmu / 2 : oneLine ? topFudge : 0;
    break;
  case OUTSIDE_SHAPE:
    topFudge = (!lines.empty()) ? lines[0].m_widthInEmu : 0;
    rightFudge = (lines.size() > 1) ? lines[1].m_widthInEmu : oneLine ? topFudge : 0;
    bottomFudge = (lines.size() > 2) ? lines[2].m_widthInEmu : oneLine ? topFudge : 0;
    leftFudge = (lines.size() > 3) ? lines[3].m_widthInEmu : oneLine ? topFudge : 0;
    break;
  case INSIDE_SHAPE:
  default:
    break;
  }
  if (makeBigger)
  {
    fudged.m_xs -= leftFudge;
    fudged.m_xe += rightFudge;
    fudged.m_ys -= topFudge;
    fudged.m_ye += bottomFudge;
  }
  else
  {
    if (int64_t(fudged.m_xe) - fudged.m_xs > leftFudge)
      fudged.m_xs += leftFudge;
    if (int64_t(fudged.m_xe) - fudged.m_xs > rightFudge)
      fudged.m_xe -= rightFudge;
    if (int64_t(fudged.m_ye) - fudged.m_ys > topFudge)
      fudged.m_ys += topFudge;
    if (int64_t(fudged.m_ye) - fudged.m_ys > bottomFudge)
      fudged.m_ye -= bottomFudge;
  }
  return fudged;
}

void MSPUBCollector::setNextPage(unsigned pageSeqNum)
{
  m_pageSeqNumsOrdered.push_back(pageSeqNum);
}

MSPUBCollector::MSPUBCollector(librevenge::RVNGDrawingInterface *painter) :
  m_painter(painter), m_contentChunkReferences(), m_width(0), m_height(0),
  m_widthSet(false), m_heightSet(false),
  m_numPages(0), m_textStringsById(), m_pagesBySeqNum(),
  m_images(), m_borderImages(),
  m_textColors(), m_fonts(),
  m_defaultCharStyles(), m_defaultParaStyles(), m_shapeTypesBySeqNum(),
  m_paletteColors(), m_shapeSeqNumsOrdered(),
  m_pageSeqNumsByShapeSeqNum(), m_bgShapeSeqNumsByPageSeqNum(),
  m_skipIfNotBgSeqNums(),
  m_currentShapeGroup(), m_topLevelShapes(),
  m_groupsBySeqNum(), m_embeddedFonts(),
  m_shapeInfosBySeqNum(), m_masterPages(),
  m_shapesWithCoordinatesRotated90(),
  m_masterPagesByPageSeqNum(),
  m_tableCellTextEndsByTextId(), m_tableBordersBySeqNum(), m_tableCellFillsBySeqNum(), m_stringOffsetsByTextId(),
  m_calculationValuesSeen(), m_pageSeqNumsOrdered(),
  m_encodingHeuristic(false), m_allText(),
  m_calculatedEncoding(),
  m_metaData()
{
}

void MSPUBCollector::setTextStringOffset(
  unsigned textId, unsigned offset)
{
  m_stringOffsetsByTextId[textId] = offset;
}

void MSPUBCollector::setShapeTextChain(unsigned seqNum, unsigned index, boost::optional<unsigned> prev, boost::optional<unsigned> next)
{
  ShapeInfo &info = m_shapeInfosBySeqNum[seqNum];
  info.m_textChainIndex = index;
  info.m_textChainPrev = prev;
  info.m_textChainNext = next;
}

void MSPUBCollector::collectTextChains()
{
  m_textChainsByTextId.clear();
  for (const auto &it : m_shapeInfosBySeqNum)
  {
    const ShapeInfo &info = it.second;
    if (bool(info.m_textId) && !bool(info.m_tableInfo))
      m_textChainsByTextId[get(info.m_textId)].push_back(std::make_pair(info.m_textChainIndex, it.first));
  }
  for (auto &it : m_textChainsByTextId)
    std::sort(it.second.begin(), it.second.end());
}

void MSPUBCollector::insertTextIdProps(const ShapeInfo &info, librevenge::RVNGPropertyList &props) const
{
  // Every text box of a story carries the whole story (unchanged libmspub
  // behaviour); these ids let a consumer lay the story out once and flow it
  // through the boxes in chain order.
  if (bool(info.m_seqNum))
    props.insert("pub:shape-id", int(get(info.m_seqNum)));
  if (!bool(info.m_textId))
    return;
  const unsigned textId = get(info.m_textId);
  props.insert("pub:story-id", int(textId));
  if (!info.m_wrapShapes.empty())
  {
    librevenge::RVNGPropertyListVector wrap;
    for (unsigned s : info.m_wrapShapes)
    {
      librevenge::RVNGPropertyList item;
      item.insert("pub:shape-id", int(s));
      wrap.append(item);
    }
    props.insert("pub:wrap-shapes", wrap);
  }
  if (bool(info.m_tableInfo))
    return;
  const auto it = m_textChainsByTextId.find(textId);
  if (it == m_textChainsByTextId.end() || !bool(info.m_seqNum))
    return;
  const auto &chain = it->second;
  size_t pos = 0;
  while (pos < chain.size() && chain[pos].second != get(info.m_seqNum))
    ++pos;
  if (pos == chain.size())
    return;
  props.insert("pub:chain-index", int(pos));
  props.insert("pub:chain-length", int(chain.size()));
  if (pos > 0)
    props.insert("pub:chain-prev", int(chain[pos - 1].second));
  if (pos + 1 < chain.size())
    props.insert("pub:chain-next", int(chain[pos + 1].second));
}

void MSPUBCollector::setTableCellTextEnds(
  const unsigned textId, const std::vector<unsigned> &ends)
{
  m_tableCellTextEndsByTextId[textId] = ends;
}

void MSPUBCollector::useEncodingHeuristic()
{
  m_encodingHeuristic = true;
}

void MSPUBCollector::setShapeShadow(unsigned seqNum, const Shadow &shadow)
{
  m_shapeInfosBySeqNum[seqNum].m_shadow = shadow;
}

void noop(const CustomShape *)
{
}

void MSPUBCollector::setShapeCoordinatesRotated90(unsigned seqNum)
{
  m_shapesWithCoordinatesRotated90.insert(seqNum);
}

void MSPUBCollector::setShapeBorderImageId(unsigned seqNum, unsigned id)
{
  m_shapeInfosBySeqNum[seqNum].m_borderImgIndex = id;
}

void MSPUBCollector::setShapeCustomPath(unsigned seqNum,
                                        const DynamicCustomShape &shape)
{
  m_shapeInfosBySeqNum[seqNum].m_customShape = shape;
}

void MSPUBCollector::setShapeClipPath(unsigned seqNum, const std::vector<Vertex> &clip)
{
  m_shapeInfosBySeqNum[seqNum].m_clipPath = clip;
}

void MSPUBCollector::beginGroup()
{
  auto tmp = ShapeGroupElement::create(m_currentShapeGroup);
  if (!m_currentShapeGroup)
  {
    m_topLevelShapes.push_back(tmp);
  }
  m_currentShapeGroup = tmp;
}

bool MSPUBCollector::endGroup()
{
  if (!m_currentShapeGroup)
  {
    return false;
  }
  m_currentShapeGroup = m_currentShapeGroup->getParent();
  return true;
}

void MSPUBCollector::addShapeLine(unsigned seqNum, Line line)
{
  m_shapeInfosBySeqNum[seqNum].m_lines.push_back(line);
}

void MSPUBCollector::setShapeBorderPosition(unsigned seqNum, BorderPosition pos)
{
  m_shapeInfosBySeqNum[seqNum].m_borderPosition = pos;
}

bool MSPUBCollector::hasPage(unsigned seqNum) const
{
  return m_pagesBySeqNum.find(seqNum) != m_pagesBySeqNum.end();
}

void MSPUBCollector::setShapeMargins(unsigned seqNum, unsigned left, unsigned top, unsigned right, unsigned bottom)
{
  m_shapeInfosBySeqNum[seqNum].m_margins = Margins(left, top, right, bottom);
}

void MSPUBCollector::setPageBgShape(unsigned pageSeqNum, unsigned seqNum)
{
  m_bgShapeSeqNumsByPageSeqNum[pageSeqNum] = seqNum;
}

bool MSPUBCollector::setCurrentGroupSeqNum(unsigned seqNum)
{
  if (!m_currentShapeGroup)
  {
    return false;
  }
  m_currentShapeGroup->setSeqNum(seqNum);
  m_groupsBySeqNum.insert(std::make_pair(seqNum, m_currentShapeGroup));
  return true;
}

void MSPUBCollector::setShapeOrder(unsigned seqNum)
{
  auto tmp = ShapeGroupElement::create(m_currentShapeGroup, seqNum);
  if (!m_currentShapeGroup)
  {
    m_topLevelShapes.push_back(tmp);
  }
}

void MSPUBCollector::addPaletteColor(Color c)
{
  m_paletteColors.push_back(c);
}

void no_op()
{
}

void endShapeGroup(librevenge::RVNGDrawingInterface *painter)
{
  painter->endLayer();
}

std::vector<int> MSPUBCollector::getShapeAdjustValues(const ShapeInfo &info) const
{
  std::vector<int> ret;
  std::shared_ptr<const CustomShape> ptr_shape = info.getCustomShape();
  if (ptr_shape)
  {
    for (unsigned i = 0; i < ptr_shape->m_numDefaultAdjustValues; ++i)
    {
      ret.push_back(ptr_shape->mp_defaultAdjustValues[i]);
    }
  }
  for (auto i = info.m_adjustValuesByIndex.begin(); i != info.m_adjustValuesByIndex.end(); ++i)
  {
    unsigned index = i->first;
    int adjustVal = i->second;
    for (unsigned j = info.m_adjustValues.size(); j <= index; ++j)
    {
      ret.push_back(0);
    }
    ret[index] = adjustVal;
  }
  return ret;
}

boost::optional<std::vector<TextParagraph> > MSPUBCollector::getShapeText(const ShapeInfo &info) const
{
  if (bool(info.m_textId))
  {
    unsigned stringId = info.m_textId.get();
    const std::vector<TextParagraph> *ptr_str = getIfExists_const(m_textStringsById, stringId);
    if (ptr_str)
    {
      return *ptr_str;
    }
  }
  return boost::optional<std::vector<TextParagraph> >();
}

void MSPUBCollector::setupShapeStructures(ShapeGroupElement &elt)
{
  ShapeInfo *ptr_info = getIfExists(m_shapeInfosBySeqNum, elt.getSeqNum());
  if (ptr_info)
  {
    if (bool(ptr_info->m_imgIndex))
    {
      unsigned index = ptr_info->m_imgIndex.get();
      int rot = 0;
      if (bool(ptr_info->m_innerRotation))
        rot = ptr_info->m_innerRotation.get();
      if (index - 1 < m_images.size())
      {
        ptr_info->m_fill = std::shared_ptr<const Fill>(new ImgFill(index, this, false, rot));
      }
    }
    ptr_info->m_seqNum = elt.getSeqNum();
    elt.setShapeInfo(*ptr_info);
    std::pair<bool, bool> flips = ptr_info->m_flips.get_value_or(std::pair<bool, bool>(false, false));
    VectorTransformation2D flipsTransform = VectorTransformation2D::fromFlips(flips.second, flips.first);
    double rotation = ptr_info->m_rotation.get_value_or(0);
    rotation = doubleModulo(rotation, 360);
    bool rotBackwards = flips.first ^ flips.second;
    VectorTransformation2D rot = VectorTransformation2D::fromCounterRadians((rotBackwards ? -rotation : rotation) * M_PI / 180);
    elt.setTransform(rot * flipsTransform);
  }
}


std::function<void(void)> MSPUBCollector::paintShape(const ShapeInfo &info, const Coordinate &/* relativeTo*/, const VectorTransformation2D &foldedTransform, bool isGroup, const VectorTransformation2D &thisTransform) const
{
  std::vector<int> adjustValues = getShapeAdjustValues(info);
  if (isGroup)
  {
    m_painter->startLayer(librevenge::RVNGPropertyList());
    return std::bind(&endShapeGroup, m_painter);
  }
  librevenge::RVNGPropertyList graphicsProps;
  if (info.m_fill)
  {
    info.m_fill->getProperties(&graphicsProps);
  }
  // pub-converter: every setStyle of a shape names the shape it belongs to
  if (bool(info.m_seqNum))
    graphicsProps.insert("pub:shape-id", int(get(info.m_seqNum)));
  // pub-converter: the shape type, and a flag when libmspub has no geometry for it
  // (writeCustomShape then draws nothing, so its fill and line are lost silently)
  {
    const unsigned shapeType = bool(info.m_cropType) ? info.m_cropType.get() : info.m_type.get_value_or(RECTANGLE);
    graphicsProps.insert("pub:shape-type", int(shapeType));
    if (!info.getCustomShape())
      graphicsProps.insert("pub:shape-undrawn", true);
  }
  // pub-converter patch 13: text wrap distances of this shape (MS-ODRAW dxWrapDist*/dyWrapDist*), when set
  {
    static const char *const names[4] = { "pub:wrap-distance-left", "pub:wrap-distance-top", "pub:wrap-distance-right", "pub:wrap-distance-bottom" };
    for (unsigned i = 0; i < 4; ++i)
      if (bool(info.m_wrapDistances[i]))
        graphicsProps.insert(names[i], double(int(get(info.m_wrapDistances[i]))) / EMUS_IN_INCH);
  }
  // pub-converter patch 15: line ends (MS-ODRAW lineStartArrowhead/lineEndArrowhead and their width/length).
  // libmspub parsed them into m_beginArrow/m_endArrow but never wrote them, so arrows lost their heads.
  {
    static const char *const styles[] = { "none", "triangle", "stealth", "diamond", "oval", "open" };
    static const char *const sizes[] = { "small", "medium", "large" };
    const boost::optional<Arrow> *arrows[2] = { &info.m_beginArrow, &info.m_endArrow };
    static const char *const keys[2] = { "pub:arrow-start", "pub:arrow-end" };
    for (unsigned i = 0; i < 2; ++i)
    {
      if (!bool(*arrows[i]) || arrows[i]->get().m_style == NO_ARROW)
        continue;
      const Arrow &a = arrows[i]->get();
      const unsigned st = unsigned(a.m_style) < 6 ? unsigned(a.m_style) : 1;
      graphicsProps.insert(keys[i], styles[st]);
      librevenge::RVNGString w, l;
      w.sprintf("%s-width", keys[i]);
      l.sprintf("%s-length", keys[i]);
      graphicsProps.insert(w.cstr(), sizes[unsigned(a.m_width) < 3 ? unsigned(a.m_width) : 1]);
      graphicsProps.insert(l.cstr(), sizes[unsigned(a.m_height) < 3 ? unsigned(a.m_height) : 1]);
    }
  }
  bool hasStroke = false;
  bool hasBorderArt = false;
  boost::optional<unsigned> maybeBorderImg = info.m_borderImgIndex;
  if (bool(maybeBorderImg) && !info.m_lines.empty())
  {
    hasStroke = true;
    hasBorderArt = true;
  }
  else
  {
    for (const auto &line : info.m_lines)
    {
      hasStroke = hasStroke || line.m_lineExists;
      if (hasStroke)
      {
        break;
      }
    }
  }
  librevenge::RVNGString fill = graphicsProps["draw:fill"] ? graphicsProps["draw:fill"]->getStr() : "none";
  if (bool(info.m_wordArt) && !get(info.m_wordArt).m_text.empty())
  {
    paintWordArt(info, graphicsProps, thisTransform);
    return &no_op;
  }
  bool hasFill = fill != "none";
  boost::optional<std::vector<TextParagraph> > maybeText = getShapeText(info);
  auto hasText = bool(maybeText);
  const auto isTable = bool(info.m_tableInfo);
  bool makeLayer = hasBorderArt ||
                   (hasStroke && hasFill) || (hasStroke && hasText) || (hasFill && hasText);
  if (makeLayer)
  {
    if (info.m_clipPath.size() > 0)
    {
      const Coordinate coord = info.m_coordinates.get_value_or(Coordinate());
      double x, y, height, width;
      x = coord.getXIn(m_width);
      y = coord.getYIn(m_height);
      height = coord.getHeightIn();
      width = coord.getWidthIn();
      m_painter->startLayer(calcClipPath(info.m_clipPath, x, y, height, width, foldedTransform, info.getCustomShape()));
    }
    else
      m_painter->startLayer(librevenge::RVNGPropertyList());
  }
  graphicsProps.insert("draw:stroke", "none");
  const Coordinate coord = info.m_coordinates.get_value_or(Coordinate());
  BorderPosition borderPosition =
    hasBorderArt ? INSIDE_SHAPE : info.m_borderPosition.get_value_or(HALF_INSIDE_SHAPE);
  unsigned type;
  if (bool(info.m_cropType))
  {
    type = info.m_cropType.get();
  }
  else
  {
    type = info.m_type.get_value_or(RECTANGLE);
  }

  if (hasFill)
  {
    double x, y, height, width;
    x = coord.getXIn(m_width);
    y = coord.getYIn(m_height);
    height = coord.getHeightIn();
    width = coord.getWidthIn();
    if (hasBorderArt)
    {
      double borderImgWidth =
        static_cast<double>(info.m_lines[0].m_widthInEmu) / EMUS_IN_INCH;
      if (height > 2 * borderImgWidth && width >= 2 * borderImgWidth)
      {
        x += borderImgWidth;
        y += borderImgWidth;
        height -= 2 * borderImgWidth;
        width -= 2 * borderImgWidth;
      }
    }
    if (bool(info.m_pictureRecolor))
    {
      Color obc = info.m_pictureRecolor.get().getFinalColor(m_paletteColors);
      graphicsProps.insert("draw:color-mode", "greyscale");
      graphicsProps.insert("draw:red",
                           static_cast<double>(obc.r) / 255.0, librevenge::RVNG_PERCENT);
      graphicsProps.insert("draw:blue",
                           static_cast<double>(obc.b) / 255.0, librevenge::RVNG_PERCENT);
      graphicsProps.insert("draw:green",
                           static_cast<double>(obc.g) / 255.0, librevenge::RVNG_PERCENT);
    }
    if (bool(info.m_pictureBrightness))
      graphicsProps.insert("draw:luminance", static_cast<double>(info.m_pictureBrightness.get() + 32768.0) / 65536.0, librevenge::RVNG_PERCENT);
    // pub-converter: picture crop and contrast (only meaningful for bitmap fills)
    const bool isBitmapFill = fill == "bitmap";
    if (isBitmapFill && bool(info.m_pictureCrop))
    {
      const std::vector<double> &crop = get(info.m_pictureCrop);
      graphicsProps.insert("pub:crop-top", crop[0]);
      graphicsProps.insert("pub:crop-bottom", crop[1]);
      graphicsProps.insert("pub:crop-left", crop[2]);
      graphicsProps.insert("pub:crop-right", crop[3]);
    }
    if (isBitmapFill && bool(info.m_pictureContrast))
      graphicsProps.insert("pub:picture-contrast", static_cast<double>(info.m_pictureContrast.get()) / 65536.0);
    bool shadowPropsInserted = false;
    if (bool(info.m_shadow))
    {
      const Shadow &s = info.m_shadow.get();
      if (!needsEmulation(s))
      {
        shadowPropsInserted = true;
        graphicsProps.insert("draw:shadow", "visible");
        graphicsProps.insert("draw:shadow-offset-x", static_cast<double>(s.m_offsetXInEmu) / EMUS_IN_INCH);
        graphicsProps.insert("draw:shadow-offset-y", static_cast<double>(s.m_offsetYInEmu) / EMUS_IN_INCH);
        graphicsProps.insert("draw:shadow-color", getColorString(s.m_color.getFinalColor(m_paletteColors)));
        graphicsProps.insert("draw:shadow-opacity", s.m_opacity, librevenge::RVNG_PERCENT);
      }
      // TODO: Emulate shadows that don't conform
      // to LibreOffice's range of possible shadows.
    }
    m_painter->setStyle(graphicsProps);

    writeCustomShape(type, graphicsProps, m_painter, x, y, height, width,
                     true, foldedTransform,
                     std::vector<Line>(), std::bind(&MSPUBCollector::getCalculationValue, this, info, _1, false, adjustValues), m_paletteColors, info.getCustomShape());
    if (bool(info.m_pictureRecolor))
    {
      graphicsProps.remove("draw:color-mode");
      graphicsProps.remove("draw:red");
      graphicsProps.remove("draw:blue");
      graphicsProps.remove("draw:green");
    }
    if (bool(info.m_pictureBrightness))
      graphicsProps.remove("draw:luminance");
    graphicsProps.remove("pub:crop-top");
    graphicsProps.remove("pub:crop-bottom");
    graphicsProps.remove("pub:crop-left");
    graphicsProps.remove("pub:crop-right");
    graphicsProps.remove("pub:picture-contrast");
    if (shadowPropsInserted)
    {
      graphicsProps.remove("draw:shadow");
      graphicsProps.remove("draw:shadow-offset-x");
      graphicsProps.remove("draw:shadow-offset-y");
      graphicsProps.remove("draw:shadow-color");
      graphicsProps.remove("draw:shadow-opacity");
    }
  }
  const std::vector<Line> &lines = info.m_lines;
  if (hasStroke)
  {
    if (hasBorderArt && lines[0].m_widthInEmu > 0)
    {
      bool stretch = info.m_stretchBorderArt;
      double x = coord.getXIn(m_width);
      double y = coord.getYIn(m_height);
      double height = coord.getHeightIn();
      double width = coord.getWidthIn();
      double borderImgWidth =
        static_cast<double>(info.m_lines[0].m_widthInEmu) / EMUS_IN_INCH;
      auto numImagesHoriz = static_cast<unsigned>(width / borderImgWidth);
      auto numImagesVert = static_cast<unsigned>(height / borderImgWidth);
      double borderVertTotalPadding = height - numImagesVert * borderImgWidth;
      double borderHorizTotalPadding = width - numImagesHoriz * borderImgWidth;
      if (numImagesHoriz >= 2 && numImagesVert >= 2)
      {
        auto numStretchedImagesVert = static_cast<unsigned>(0.5 + (height - 2 * borderImgWidth) / borderImgWidth);
        auto numStretchedImagesHoriz = static_cast<unsigned>(0.5 + (width - 2 * borderImgWidth) / borderImgWidth);
        double stretchedImgHeight = stretch ?
                                    (height - 2 * borderImgWidth) / numStretchedImagesVert :
                                    borderImgWidth;
        double stretchedImgWidth = stretch ?
                                   (width - 2 * borderImgWidth) / numStretchedImagesHoriz :
                                   borderImgWidth;
        if (stretch)
        {
          numImagesVert = 2 + numStretchedImagesVert;
          numImagesHoriz = 2 + numStretchedImagesHoriz;
        }
        double borderVertPadding = borderVertTotalPadding / (numImagesVert - 1);
        double borderHorizPadding = borderHorizTotalPadding / (numImagesHoriz - 1);
        if (maybeBorderImg.get() < m_borderImages.size())
        {
          const BorderArtInfo &ba = m_borderImages[maybeBorderImg.get()];
          if (!ba.m_offsets.empty())
          {
            librevenge::RVNGPropertyList baProps;
            baProps.insert("draw:stroke", "none");
            baProps.insert("draw:fill", "solid");
            baProps.insert("draw:fill-color", "#ffffff");
            m_painter->setStyle(baProps);
            librevenge::RVNGPropertyList topRectProps;
            topRectProps.insert("svg:x", x);
            topRectProps.insert("svg:y", y);
            topRectProps.insert("svg:height", borderImgWidth);
            topRectProps.insert("svg:width", width);
            m_painter->drawRectangle(topRectProps);
            librevenge::RVNGPropertyList rightRectProps;
            rightRectProps.insert("svg:x", x + width - borderImgWidth);
            rightRectProps.insert("svg:y", y);
            rightRectProps.insert("svg:height", height);
            rightRectProps.insert("svg:width", borderImgWidth);
            m_painter->drawRectangle(rightRectProps);
            librevenge::RVNGPropertyList botRectProps;
            botRectProps.insert("svg:x", x);
            botRectProps.insert("svg:y", y + height - borderImgWidth);
            botRectProps.insert("svg:height", borderImgWidth);
            botRectProps.insert("svg:width", width);
            m_painter->drawRectangle(botRectProps);
            librevenge::RVNGPropertyList leftRectProps;
            leftRectProps.insert("svg:x", x);
            leftRectProps.insert("svg:y", y);
            leftRectProps.insert("svg:height", height);
            leftRectProps.insert("svg:width", borderImgWidth);
            m_painter->drawRectangle(leftRectProps);
            auto iOffset = ba.m_offsets.begin();
            boost::optional<Color> oneBitColor;
            if (bool(info.m_lineBackColor))
            {
              oneBitColor = info.m_lineBackColor.get().getFinalColor(m_paletteColors);
            }
            // top left
            unsigned iOrdOff = find(ba.m_offsetsOrdered.begin(),
                                    ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              writeImage(x, y, borderImgWidth, borderImgWidth,
                         bi.m_type, bi.m_imgBlob, oneBitColor);
            }
            if (iOffset + 1 != ba.m_offsets.end())
            {
              ++iOffset;
            }
            // top
            iOrdOff = find(ba.m_offsetsOrdered.begin(),
                           ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              for (unsigned iTop = 1; iTop < numImagesHoriz - 1; ++iTop)
              {
                double imgX = stretch ?
                              x + borderImgWidth + (iTop - 1) * stretchedImgWidth :
                              x + iTop * (borderImgWidth + borderHorizPadding);
                writeImage(imgX, y,
                           borderImgWidth, stretchedImgWidth,
                           bi.m_type, bi.m_imgBlob, oneBitColor);
              }
            }
            if (iOffset + 1 != ba.m_offsets.end())
            {
              ++iOffset;
            }
            // top right
            iOrdOff = find(ba.m_offsetsOrdered.begin(),
                           ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              writeImage(x + width - borderImgWidth, y,
                         borderImgWidth, borderImgWidth,
                         bi.m_type, bi.m_imgBlob, oneBitColor);
            }
            if (iOffset + 1 != ba.m_offsets.end())
            {
              ++iOffset;
            }
            // right
            iOrdOff = find(ba.m_offsetsOrdered.begin(),
                           ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              for (unsigned iRight = 1; iRight < numImagesVert - 1; ++iRight)
              {
                double imgY = stretch ?
                              y + borderImgWidth + (iRight - 1) * stretchedImgHeight :
                              y + iRight * (borderImgWidth + borderVertPadding);
                writeImage(x + width - borderImgWidth,
                           imgY,
                           stretchedImgHeight, borderImgWidth,
                           bi.m_type, bi.m_imgBlob, oneBitColor);
              }
            }
            if (iOffset + 1 != ba.m_offsets.end())
            {
              ++iOffset;
            }
            // bottom right
            iOrdOff = find(ba.m_offsetsOrdered.begin(),
                           ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              writeImage(x + width - borderImgWidth,
                         y + height - borderImgWidth,
                         borderImgWidth, borderImgWidth,
                         bi.m_type, bi.m_imgBlob, oneBitColor);
            }
            if (iOffset + 1 != ba.m_offsets.end())
            {
              ++iOffset;
            }
            // bottom
            iOrdOff = find(ba.m_offsetsOrdered.begin(),
                           ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              for (unsigned iBot = 1; iBot < numImagesHoriz - 1; ++iBot)
              {
                double imgX = stretch ?
                              x + width - borderImgWidth - iBot * stretchedImgWidth :
                              x + width - borderImgWidth - iBot * (borderImgWidth + borderHorizPadding);
                writeImage(
                  imgX, y + height - borderImgWidth,
                  borderImgWidth, stretchedImgWidth,
                  bi.m_type, bi.m_imgBlob, oneBitColor);
              }
            }
            if (iOffset + 1 != ba.m_offsets.end())
            {
              ++iOffset;
            }
            // bottom left
            iOrdOff = find(ba.m_offsetsOrdered.begin(),
                           ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              writeImage(x, y + height - borderImgWidth,
                         borderImgWidth, borderImgWidth,
                         bi.m_type, bi.m_imgBlob, oneBitColor);
            }
            if (iOffset + 1 != ba.m_offsets.end())
            {
              ++iOffset;
            }
            // left
            iOrdOff = find(ba.m_offsetsOrdered.begin(),
                           ba.m_offsetsOrdered.end(), *iOffset) - ba.m_offsetsOrdered.begin();
            if (iOrdOff < ba.m_images.size())
            {
              const BorderImgInfo &bi = ba.m_images[iOrdOff];
              for (unsigned iLeft = 1; iLeft < numImagesVert - 1; ++iLeft)
              {
                double imgY = stretch ?
                              y + height - borderImgWidth - iLeft * stretchedImgHeight :
                              y + height - borderImgWidth -
                              iLeft * (borderImgWidth + borderVertPadding);
                writeImage(x, imgY, stretchedImgHeight, borderImgWidth,
                           bi.m_type, bi.m_imgBlob, oneBitColor);
              }
            }
          }
        }
      }
    }
    else
    {
      Coordinate strokeCoord = isShapeTypeRectangle(type) ?
                               getFudgedCoordinates(coord, lines, true, borderPosition) : coord;
      double x, y, height, width;
      x = strokeCoord.getXIn(m_width);
      y = strokeCoord.getYIn(m_height);
      height = strokeCoord.getHeightIn();
      width = strokeCoord.getWidthIn();
      graphicsProps.insert("draw:fill", "none");
      if (bool(info.m_dash) && !info.m_dash.get().m_dots.empty())
      {
        const Dash &dash = info.m_dash.get();
        graphicsProps.insert("draw:stroke", "dash");
        graphicsProps.insert("draw:distance", dash.m_distance, librevenge::RVNG_INCH);
        switch (dash.m_dotStyle)
        {
        case ROUND_DOT:
          graphicsProps.insert("svg:stroke-linecap", "round");
          break;
        case RECT_DOT:
          graphicsProps.insert("svg:stroke-linecap", "butt");
          break;
        default:
          break;
        }
        for (size_t i = 0; i < dash.m_dots.size(); ++i)
        {
          librevenge::RVNGString dots;
          dots.sprintf("draw:dots%ld", i + 1);
          graphicsProps.insert(dots.cstr(), static_cast<int>(dash.m_dots[i].m_count));
          if (bool(dash.m_dots[i].m_length))
          {
            librevenge::RVNGString length;
            length.sprintf("draw:dots%ld-length", i + 1);
            graphicsProps.insert(length.cstr(), dash.m_dots[i].m_length.get(), librevenge::RVNG_INCH);
          }
        }
      }
      else
      {
        graphicsProps.insert("draw:stroke", "solid");
      }
      m_painter->setStyle(graphicsProps);
      writeCustomShape(type, graphicsProps, m_painter, x, y, height, width,
                       false, foldedTransform, lines,
                       std::bind(
                         &MSPUBCollector::getCalculationValue, this, info, _1, false, adjustValues
                       ),
                       m_paletteColors, info.getCustomShape());
    }
  }
  if (hasText)
  {
    const std::vector<TextParagraph> &text = maybeText.get();
    graphicsProps.insert("draw:fill", "none");
    Coordinate textCoord = isShapeTypeRectangle(type) ?
                           getFudgedCoordinates(coord, lines, false, borderPosition) : coord;
    m_painter->setStyle(graphicsProps);
    librevenge::RVNGPropertyList props;
    setRectCoordProps(textCoord, &props);
    double textRotation = thisTransform.getRotation();
    if (textRotation != 0)
    {
      props.insert("librevenge:rotate", textRotation * 180 / M_PI);
    }

    if (isTable)
    {
      librevenge::RVNGPropertyListVector columnWidths;
      for (unsigned int col : get(info.m_tableInfo).m_columnWidthsInEmu)
      {
        librevenge::RVNGPropertyList columnWidth;
        columnWidth.insert("style:column-width", double(col) / EMUS_IN_INCH);
        columnWidths.append(columnWidth);
      }
      props.insert("librevenge:table-columns", columnWidths);
      insertTextIdProps(info, props);
      // pub-converter patch 13: border segments on the table grid (see engine/JSON.md)
      if (bool(info.m_seqNum))
      {
        const auto bit = m_tableBordersBySeqNum.find(get(info.m_seqNum));
        if (bit != m_tableBordersBySeqNum.end())
        {
          librevenge::RVNGPropertyListVector borders;
          for (const TableBorderInfo &b : bit->second)
          {
            if (!b.m_visible || (b.m_kind != 1 && b.m_kind != 2))
              continue;
            librevenge::RVNGPropertyList seg;
            seg.insert("pub:border-dir", b.m_kind == 1 ? "h" : "v");
            seg.insert("pub:row1", int(b.m_row1));
            seg.insert("pub:col1", int(b.m_col1));
            seg.insert("pub:row2", int(b.m_row2));
            seg.insert("pub:col2", int(b.m_col2));
            seg.insert("svg:stroke-color", getColorString(ColorReference(b.m_color).getFinalColor(m_paletteColors)));
            seg.insert("svg:stroke-width", double(b.m_widthEmu) / EMUS_IN_INCH);
            if (bool(b.m_dashing) && get(b.m_dashing) != 0)
            {
              seg.insert("draw:stroke", "dash");
              seg.insert("pub:line-dashing", int(get(b.m_dashing)));
            }
            else
              seg.insert("draw:stroke", "solid");
            borders.append(seg);
          }
          if (borders.count())
            props.insert("pub:table-borders", borders);
        }
      }

      m_painter->startTableObject(props);

      const std::map<unsigned, std::vector<unsigned> >::const_iterator it = m_tableCellTextEndsByTextId.find(get(info.m_textId));
      const std::vector<unsigned> &tableCellTextEnds = (it != m_tableCellTextEndsByTextId.end()) ? it->second : std::vector<unsigned>();

      TableLayout tableLayout(boost::extents[get(info.m_tableInfo).m_numRows][get(info.m_tableInfo).m_numColumns]);
      createTableLayout(get(info.m_tableInfo).m_cells, tableLayout);

      ParagraphToCellMap_t paraToCellMap;
      ParagraphTexts_t paraTexts;
      mapTableTextToCells(text, tableCellTextEnds, getCalculatedEncoding(), paraToCellMap, paraTexts);

      for (unsigned row = 0; row != tableLayout.shape()[0]; ++row)
      {
        librevenge::RVNGPropertyList rowProps;
        if (row < (get(info.m_tableInfo).m_rowHeightsInEmu.size()))
          rowProps.insert("librevenge:row-height", double(get(info.m_tableInfo).m_rowHeightsInEmu[row]) / EMUS_IN_INCH);
        m_painter->openTableRow(rowProps);

        for (unsigned col = 0; col != tableLayout.shape()[1]; ++col)
        {
          librevenge::RVNGPropertyList cellProps;
          cellProps.insert("librevenge:column", int(col));
          cellProps.insert("librevenge:row", int(row));

          if (isCovered(tableLayout[row][col]))
          {
            m_painter->insertCoveredTableCell(cellProps);
          }
          else
          {
            if (tableLayout[row][col].m_colSpan > 1)
              cellProps.insert("table:number-columns-spanned", int(tableLayout[row][col].m_colSpan));
            if (tableLayout[row][col].m_rowSpan > 1)
              cellProps.insert("table:number-rows-spanned", int(tableLayout[row][col].m_rowSpan));
            // pub-converter patch 13: cell fill and text margins
            {
              const unsigned cellIndex = tableLayout[row][col].m_cell;
              const std::vector<CellInfo> &cells = get(info.m_tableInfo).m_cells;
              if (cellIndex < cells.size())
              {
                static const char *const pad[4] = { "fo:padding-left", "fo:padding-right", "fo:padding-top", "fo:padding-bottom" };
                for (unsigned i = 0; i < 4; ++i)
                  if (bool(cells[cellIndex].m_marginsEmu[i]))
                    cellProps.insert(pad[i], double(int(get(cells[cellIndex].m_marginsEmu[i]))) / EMUS_IN_INCH);
              }
              if (bool(info.m_seqNum))
              {
                const auto fit = m_tableCellFillsBySeqNum.find(get(info.m_seqNum));
                if (fit != m_tableCellFillsBySeqNum.end())
                {
                  for (const TableCellFillInfo &f : fit->second)
                  {
                    if (f.m_cellIndex != cellIndex || !f.m_filled)
                      continue;
                    cellProps.insert("fo:background-color", getColorString(ColorReference(f.m_color).getFinalColor(m_paletteColors)));
                    if (bool(f.m_opacity) && get(f.m_opacity) < 0x10000)
                      cellProps.insert("draw:opacity", double(get(f.m_opacity)) / 0x10000, librevenge::RVNG_PERCENT);
                  }
                }
              }
            }

            m_painter->openTableCell(cellProps);

            if (tableLayout[row][col].m_cell < paraToCellMap.size())
            {
              const std::pair<unsigned, unsigned> &cellParas = paraToCellMap[tableLayout[row][col].m_cell];
              for (unsigned para = cellParas.first; para <= cellParas.second; ++para)
              {
                librevenge::RVNGPropertyList paraProps = getParaStyleProps(text[para].style, text[para].style.m_defaultCharStyleIndex);
                m_painter->openParagraph(paraProps);

                for (size_t i_spans = 0; i_spans < paraTexts[para].size(); ++i_spans)
                {
                  librevenge::RVNGPropertyList charProps = getCharStyleProps(text[para].spans[i_spans].style, text[para].style.m_defaultCharStyleIndex);
                  m_painter->openSpan(charProps);
                  insertTextWithFields(m_painter, paraTexts[para][i_spans]);
                  m_painter->closeSpan();
                }

                m_painter->closeParagraph();
              }
            }

            m_painter->closeTableCell();
          }
        }

        m_painter->closeTableRow();
      }

      m_painter->endTableObject();
    }
    else // a text object
    {
      Margins margins = info.m_margins.get_value_or(Margins());
      props.insert("fo:padding-left", (double)margins.m_left / EMUS_IN_INCH);
      props.insert("fo:padding-top", (double)margins.m_top / EMUS_IN_INCH);
      props.insert("fo:padding-right", (double)margins.m_right / EMUS_IN_INCH);
      props.insert("fo:padding-bottom", (double)margins.m_bottom / EMUS_IN_INCH);
      if (bool(info.m_verticalAlign))
      {
        switch (info.m_verticalAlign.get())
        {
        default:
        case TOP:
          props.insert("draw:textarea-vertical-align", "top");
          break;
        case MIDDLE:
          props.insert("draw:textarea-vertical-align", "middle");
          break;
        case BOTTOM:
          props.insert("draw:textarea-vertical-align", "bottom");
          break;
        }
      }
      if (info.m_numColumns)
      {
        unsigned ncols = info.m_numColumns.get_value_or(0);
        if (ncols > 0)
          props.insert("fo:column-count", (int)ncols);
      }
      if (info.m_columnSpacing)
      {
        unsigned ngap = info.m_columnSpacing;
        if (ngap > 0)
          props.insert("fo:column-gap", (double)ngap / EMUS_IN_INCH);
      }
      insertTextIdProps(info, props);
      m_painter->startTextObject(props);
      for (const auto &line : text)
      {
        librevenge::RVNGPropertyList paraProps = getParaStyleProps(line.style, line.style.m_defaultCharStyleIndex);
        m_painter->openParagraph(paraProps);
        for (size_t i_spans = 0; i_spans < line.spans.size(); ++i_spans)
        {
          librevenge::RVNGString textString;
          appendCharacters(textString, line.spans[i_spans].chars,
                           getCalculatedEncoding());
          librevenge::RVNGPropertyList charProps = getCharStyleProps(line.spans[i_spans].style, line.style.m_defaultCharStyleIndex);
          m_painter->openSpan(charProps);
          insertTextWithFields(m_painter, textString);
          m_painter->closeSpan();
        }
        m_painter->closeParagraph();
      }
      m_painter->endTextObject();
    }
  }
  if (makeLayer)
  {
    m_painter->endLayer();
  }
  return &no_op;
}

const char *MSPUBCollector::getCalculatedEncoding() const
{
  if (bool(m_calculatedEncoding))
  {
    return m_calculatedEncoding.get();
  }
  // modern versions are somewhat sane and use Unicode
  if (! m_encodingHeuristic)
  {
    m_calculatedEncoding = "UTF-16LE";
    return m_calculatedEncoding.get();
  }
  // for older versions of PUB, see if we can get ICU to tell us the encoding.
  UErrorCode status = U_ZERO_ERROR;
  UCharsetDetector *ucd = nullptr;
  const UCharsetMatch **matches = nullptr;
  const UCharsetMatch *ucm = nullptr;
  ucd = ucsdet_open(&status);
  int matchesFound = -1;
  const char *name = nullptr;
  const char *windowsName = nullptr;
  if (m_allText.empty())
  {
    goto csd_fail;
  }
  if (U_FAILURE(status))
  {
    goto csd_fail;
  }
  // don't worry, the below call doesn't require a null-terminated string.
  ucsdet_setText(ucd, (const char *)m_allText.data(), m_allText.size(), &status);
  if (U_FAILURE(status))
  {
    goto csd_fail;
  }
  matches = ucsdet_detectAll(ucd, &matchesFound, &status);
  if (U_FAILURE(status))
  {
    goto csd_fail;
  }
  //find best fit that is an actual Windows encoding
  for (int i = 0; i < matchesFound; ++i)
  {
    ucm = matches[i];
    name = ucsdet_getName(ucm, &status);
    if (U_FAILURE(status))
    {
      goto csd_fail;
    }
    windowsName = windowsCharsetNameByOriginalCharset(name);
    if (windowsName)
    {
      m_calculatedEncoding = windowsName;
      ucsdet_close(ucd);
      return windowsName;
    }
  }
csd_fail:
  ucsdet_close(ucd);
  // Pretty likely to give garbage text, but it's the best we can do.
  m_calculatedEncoding = "windows-1252";
  return m_calculatedEncoding.get();
}

void MSPUBCollector::setShapeLineBackColor(unsigned shapeSeqNum,
                                           ColorReference backColor)
{
  m_shapeInfosBySeqNum[shapeSeqNum].m_lineBackColor = backColor;
}

void MSPUBCollector::writeImage(double x, double y,
                                double height, double width, ImgType type, const librevenge::RVNGBinaryData &blob,
                                boost::optional<Color> oneBitColor) const
{
  librevenge::RVNGPropertyList props;
  if (bool(oneBitColor))
  {
    Color obc = oneBitColor.get();
    props.insert("draw:color-mode", "greyscale");
    props.insert("draw:red", static_cast<double>(obc.r) / 255.0, librevenge::RVNG_PERCENT);
    props.insert("draw:blue", static_cast<double>(obc.b) / 255.0, librevenge::RVNG_PERCENT);
    props.insert("draw:green", static_cast<double>(obc.g) / 255.0, librevenge::RVNG_PERCENT);
  }
  props.insert("svg:x", x);
  props.insert("svg:y", y);
  props.insert("svg:width", width);
  props.insert("svg:height", height);
  props.insert("librevenge:mime-type", mimeByImgType(type));
  props.insert("office:binary-data", blob);
  m_painter->drawGraphicObject(props);
}

double MSPUBCollector::getSpecialValue(const ShapeInfo &info, const CustomShape &shape, int arg, const std::vector<int> &adjustValues) const
{
  if (PROP_ADJUST_VAL_FIRST <= arg && PROP_ADJUST_VAL_LAST >= arg)
  {
    unsigned adjustIndex = arg - PROP_ADJUST_VAL_FIRST;
    if (adjustIndex < adjustValues.size())
    {
      if ((shape.m_adjustShiftMask >> adjustIndex) & 0x1)
      {
        return adjustValues[adjustIndex] >> 16;
      }
      return adjustValues[adjustIndex];
    }
    return 0;
  }
  if (arg == ASPECT_RATIO)
  {
    const Coordinate coord = info.m_coordinates.get_value_or(Coordinate());
    return coord.getHeightIn() != 0 ? double(coord.getWidthIn()) / coord.getHeightIn() : 0;
  }
  if (arg & OTHER_CALC_VAL)
  {
    return getCalculationValue(info, arg & 0xff, true, adjustValues);
  }
  switch (arg)
  {
  case PROP_GEO_LEFT:
    return 0;
  case PROP_GEO_TOP:
    return 0;
  case PROP_GEO_RIGHT:
    return shape.m_coordWidth;
  case PROP_GEO_BOTTOM:
    return shape.m_coordHeight;
  default:
    break;
  }
  return 0;
}

double MSPUBCollector::getCalculationValue(const ShapeInfo &info, unsigned index, bool recursiveEntry, const std::vector<int> &adjustValues) const
{
  std::shared_ptr<const CustomShape> p_shape = info.getCustomShape();
  if (! p_shape)
  {
    return 0;
  }
  const CustomShape &shape = *p_shape;
  if (index >= shape.m_numCalculations)
  {
    return 0;
  }
  if (! recursiveEntry)
  {
    m_calculationValuesSeen.clear();
    m_calculationValuesSeen.resize(shape.m_numCalculations);
  }
  if (m_calculationValuesSeen[index])
  {
    //recursion detected. The simplest way to avoid infinite recursion, at the "cost"
    // of making custom shape parsing not Turing-complete ;), is to ban recursion entirely.
    return 0;
  }
  m_calculationValuesSeen[index] = true;

  const Calculation &c = shape.mp_calculations[index];
  bool oneSpecial = (c.m_flags & 0x2000) != 0;
  bool twoSpecial = (c.m_flags & 0x4000) != 0;
  bool threeSpecial = (c.m_flags & 0x8000) != 0;

  double valOne = oneSpecial ? getSpecialValue(info, shape, c.m_argOne, adjustValues) : c.m_argOne;
  double valTwo = twoSpecial ? getSpecialValue(info, shape, c.m_argTwo, adjustValues) : c.m_argTwo;
  double valThree = threeSpecial ? getSpecialValue(info, shape, c.m_argThree, adjustValues) : c.m_argThree;
  m_calculationValuesSeen[index] = false;
  switch (c.m_flags & 0xFF)
  {
  case 0:
  case 14:
    return valOne + valTwo - valThree;
  case 1:
    return valOne * valTwo / (valThree == 0 ? 1 : valThree);
  case 2:
    return (valOne + valTwo) / 2;
  case 3:
    return fabs(valOne);
  case 4:
    return std::min(valOne, valTwo);
  case 5:
    return std::max(valOne, valTwo);
  case 6:
    return valOne ? valTwo : valThree;
  case 7:
    return sqrt(valOne * valTwo * valThree);
  case 8:
    return atan2(valTwo, valOne) / (M_PI / 180);
  case 9:
    return valOne * sin(valTwo * (M_PI / 180));
  case 10:
    return valOne * cos(valTwo * (M_PI / 180));
  case 11:
    return valOne * cos(atan2(valThree, valTwo));
  case 12:
    return valOne * sin(atan2(valThree, valTwo));
  case 13:
    return sqrt(valOne);
  case 15:
    return valThree * sqrt(1 - (valOne / valTwo) * (valOne / valTwo));
  case 16:
    return valOne * tan(valTwo);
  case 0x80:
    return sqrt(valThree * valThree - valOne * valOne);
  case 0x81:
    return (cos(valThree * (M_PI / 180)) * (valOne - 10800) + sin(valThree * (M_PI / 180)) * (valTwo - 10800)) + 10800;
  case 0x82:
    return -(sin(valThree * (M_PI / 180)) * (valOne - 10800) - cos(valThree * (M_PI / 180)) * (valTwo - 10800)) + 10800;
  default:
    return 0;
  }
}

MSPUBCollector::~MSPUBCollector()
{
}

void MSPUBCollector::setShapeRotation(unsigned seqNum, double rotation)
{
  m_shapeInfosBySeqNum[seqNum].m_rotation = rotation;
  m_shapeInfosBySeqNum[seqNum].m_innerRotation = (int)rotation;
}

void MSPUBCollector::setShapeFlip(unsigned seqNum, bool flipVertical, bool flipHorizontal)
{
  m_shapeInfosBySeqNum[seqNum].m_flips = std::pair<bool, bool>(flipVertical, flipHorizontal);
}

void MSPUBCollector::setShapeType(unsigned seqNum, unsigned type)
{
  m_shapeInfosBySeqNum[seqNum].m_type = type;
}

void MSPUBCollector::setAdjustValue(unsigned seqNum, unsigned index, int adjust)
{
  m_shapeInfosBySeqNum[seqNum].m_adjustValuesByIndex[index] = adjust;
}

void MSPUBCollector::addDefaultCharacterStyle(const CharacterStyle &st)
{
  m_defaultCharStyles.push_back(st);
}

void MSPUBCollector::setDefaultCharStyleSizeIfUnset(unsigned index, double sizePt)
{
  if (index < m_defaultCharStyles.size() && !bool(m_defaultCharStyles[index].textSizeInPt))
    m_defaultCharStyles[index].textSizeInPt = sizePt;
}

void MSPUBCollector::addDefaultParagraphStyle(const ParagraphStyle &st)
{
  m_defaultParaStyles.push_back(st);
}

bool MSPUBCollector::addPage(unsigned seqNum)
{
  if (!(m_widthSet && m_heightSet))
  {
    return false;
  }
  MSPUB_DEBUG_MSG(("Adding page of seqnum 0x%x\n", seqNum));
  m_pagesBySeqNum[seqNum] = PageInfo();
  return true;
}

void MSPUBCollector::addTextShape(unsigned stringId, unsigned seqNum)
{
  m_shapeInfosBySeqNum[seqNum].m_textId = stringId;
}

void MSPUBCollector::setShapeImgIndex(unsigned seqNum, unsigned index)
{
  MSPUB_DEBUG_MSG(("Setting image index of shape with seqnum 0x%x to 0x%x\n", seqNum, index));
  m_shapeInfosBySeqNum[seqNum].m_imgIndex = index;
}

void MSPUBCollector::setShapeDash(unsigned seqNum, const Dash &dash)
{
  m_shapeInfosBySeqNum[seqNum].m_dash = dash;
}

void MSPUBCollector::setShapeFill(unsigned seqNum, std::shared_ptr<Fill> fill, bool skipIfNotBg)
{
  m_shapeInfosBySeqNum[seqNum].m_fill = fill;
  if (skipIfNotBg)
  {
    m_skipIfNotBgSeqNums.insert(seqNum);
  }
}

void MSPUBCollector::setShapeCoordinatesInEmu(unsigned seqNum, int xs, int ys, int xe, int ye)
{
  m_shapeInfosBySeqNum[seqNum].m_coordinates = Coordinate(xs, ys, xe, ye);
}

void MSPUBCollector::addFont(std::vector<unsigned char> name)
{
  m_fonts.push_back(name);
}

librevenge::RVNGPropertyList MSPUBCollector::getParaStyleProps(const ParagraphStyle &style, boost::optional<unsigned> defaultParaStyleIndex) const
{
  ParagraphStyle _nothing;
  // pub-converter: like getCharStyleProps, a paragraph without a style index uses style 0 ("Normal").
  // Upstream used no default at all, so Normal's line spacing (1.19 sp in Publisher 2010+ templates) and
  // space after were lost for every paragraph that only inherits them.
  if (!defaultParaStyleIndex)
    defaultParaStyleIndex = 0;
  const ParagraphStyle &defaultStyle = defaultParaStyleIndex.get() < m_defaultParaStyles.size() ? m_defaultParaStyles[defaultParaStyleIndex.get()] : _nothing;
  librevenge::RVNGPropertyList ret;
  Alignment align = style.m_align.get_value_or(
                      defaultStyle.m_align.get_value_or(LEFT));
  switch (align)
  {
  case RIGHT:
    ret.insert("fo:text-align", "right");
    break;
  case CENTER:
    ret.insert("fo:text-align", "center");
    break;
  case JUSTIFY:
    ret.insert("fo:text-align", "justify");
    break;
  case LEFT:
  default:
    ret.insert("fo:text-align", "left");
    break;
  }
  LineSpacingInfo info = style.m_lineSpacing.get_value_or(
                           defaultStyle.m_lineSpacing.get_value_or(LineSpacingInfo()));
  LineSpacingType lineSpacingType = info.m_type;
  double lineSpacing = info.m_amount;
  if (!(lineSpacingType == LINE_SPACING_SP && lineSpacing == 1))
  {
    if (lineSpacingType == LINE_SPACING_SP)
    {
      ret.insert("fo:line-height", lineSpacing, librevenge::RVNG_PERCENT);
    }
    else if (lineSpacingType == LINE_SPACING_PT)
    {
      ret.insert("fo:line-height", lineSpacing, librevenge::RVNG_POINT);
    }
  }
  unsigned spaceAfterEmu = style.m_spaceAfterEmu.get_value_or(
                             defaultStyle.m_spaceAfterEmu.get_value_or(0));
  unsigned spaceBeforeEmu = style.m_spaceBeforeEmu.get_value_or(
                              defaultStyle.m_spaceBeforeEmu.get_value_or(0));
  int firstLineIndentEmu = style.m_firstLineIndentEmu.get_value_or(
                             defaultStyle.m_firstLineIndentEmu.get_value_or(0));
  unsigned leftIndentEmu = style.m_leftIndentEmu.get_value_or(
                             defaultStyle.m_leftIndentEmu.get_value_or(0));
  unsigned rightIndentEmu = style.m_rightIndentEmu.get_value_or(
                              defaultStyle.m_rightIndentEmu.get_value_or(0));
  if (spaceAfterEmu != 0)
  {
    ret.insert("fo:margin-bottom", (double)spaceAfterEmu / EMUS_IN_INCH);
  }
  if (spaceBeforeEmu != 0)
  {
    ret.insert("fo:margin-top", (double)spaceBeforeEmu / EMUS_IN_INCH);
  }
  if (firstLineIndentEmu != 0)
  {
    ret.insert("fo:text-indent", (double)firstLineIndentEmu / EMUS_IN_INCH);
  }
  if (leftIndentEmu != 0)
  {
    ret.insert("fo:margin-left", (double)leftIndentEmu / EMUS_IN_INCH);
  }
  if (rightIndentEmu != 0)
  {
    ret.insert("fo:margin-right", (double)rightIndentEmu / EMUS_IN_INCH);
  }
  unsigned dropCapLines = style.m_dropCapLines.get_value_or(
                            defaultStyle.m_dropCapLines.get_value_or(0));
  if (dropCapLines != 0)
  {
    ret.insert("style:drop-cap", (int)dropCapLines);
  }
  unsigned dropCapLetters = style.m_dropCapLetters.get_value_or(
                              defaultStyle.m_dropCapLetters.get_value_or(0));
  if (dropCapLetters != 0)
  {
    ret.insert("style:length", (int)dropCapLetters);
  }
  // pub-converter: tab stops (positions were parsed but dropped before)
  const ParagraphStyle &tabSource = !style.m_tabStopsInEmu.empty() ? style : defaultStyle;
  if (!tabSource.m_tabStopsInEmu.empty())
  {
    librevenge::RVNGPropertyListVector tabs;
    for (size_t i = 0; i < tabSource.m_tabStopsInEmu.size(); ++i)
    {
      librevenge::RVNGPropertyList tab;
      tab.insert("style:position", double(tabSource.m_tabStopsInEmu[i]) / EMUS_IN_INCH);
      const unsigned flags = i < tabSource.m_tabStopFlags.size() ? tabSource.m_tabStopFlags[i] : 0;
      // [guess] same codes as Publisher paragraph alignment (0 left, 1 right, 2 center);
      // 3 = decimal is a further guess. Seen in tdf89393: 2 at the box middle, 1 at its right edge.
      switch (flags & 0xff)
      {
      case 1:
        tab.insert("style:type", "right");
        break;
      case 2:
        tab.insert("style:type", "center");
        break;
      case 3:
        tab.insert("style:type", "char");
        tab.insert("style:char", ".");
        break;
      default:
        tab.insert("style:type", "left");
        break;
      }
      tab.insert("pub:tab-flags", int(flags));
      tabs.append(tab);
    }
    ret.insert("style:tab-stops", tabs);
  }
  // pub-converter: list info (bullets / numbering), see OUR-PATCHES.md
  const boost::optional<ListInfo> &list = bool(style.m_listInfo) ? style.m_listInfo : defaultStyle.m_listInfo;
  if (bool(list))
  {
    const ListInfo &li = get(list);
    const unsigned fmt = li.m_formatCode;
    ret.insert("pub:list-format", int(fmt));
    if (fmt == 255)
    {
      ret.insert("pub:list-type", "none");
    }
    else if (fmt == 23)
    {
      ret.insert("pub:list-type", "bullet");
      if (li.m_bulletCode && li.m_bulletCode < 0x110000 && !(li.m_bulletCode >= 0xD800 && li.m_bulletCode < 0xE000))
      {
        ret.insert("pub:list-bullet-code", int(li.m_bulletCode));
        librevenge::RVNGString ch;
        const unsigned cp = li.m_bulletCode;
        char buf[5] = {0, 0, 0, 0, 0};
        if (cp < 0x80)
          buf[0] = char(cp);
        else if (cp < 0x800)
        {
          buf[0] = char(0xC0 | (cp >> 6));
          buf[1] = char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
          buf[0] = char(0xE0 | (cp >> 12));
          buf[1] = char(0x80 | ((cp >> 6) & 0x3F));
          buf[2] = char(0x80 | (cp & 0x3F));
        }
        else
        {
          buf[0] = char(0xF0 | (cp >> 18));
          buf[1] = char(0x80 | ((cp >> 12) & 0x3F));
          buf[2] = char(0x80 | ((cp >> 6) & 0x3F));
          buf[3] = char(0x80 | (cp & 0x3F));
        }
        ch.append(buf);
        ret.insert("pub:list-bullet-char", ch);
      }
    }
    else
    {
      ret.insert("pub:list-type", "number");
      if (bool(li.m_numberIfRestarted))
        ret.insert("pub:list-start", int(get(li.m_numberIfRestarted)));
    }
    if (bool(li.m_delimiterRaw))
      ret.insert("pub:list-delimiter-raw", int(get(li.m_delimiterRaw)));
    if (bool(li.m_raw2))
      ret.insert("pub:list-raw2", int(get(li.m_raw2)));
  }
  return ret;
}

librevenge::RVNGPropertyList MSPUBCollector::getCharStyleProps(const CharacterStyle &style, boost::optional<unsigned> defaultCharStyleIndex) const
{
  CharacterStyle _nothing;
  if (!defaultCharStyleIndex)
  {
    defaultCharStyleIndex = 0;
  }
  const CharacterStyle &defaultCharStyle = defaultCharStyleIndex.get() < m_defaultCharStyles.size() ? m_defaultCharStyles[defaultCharStyleIndex.get()] : _nothing;
  librevenge::RVNGPropertyList ret;
  if (style.italic ^ defaultCharStyle.italic)
  {
    ret.insert("fo:font-style", "italic");
  }
  if (style.bold ^ defaultCharStyle.bold)
  {
    ret.insert("fo:font-weight", "bold");
  }
  if (style.outline ^ defaultCharStyle.outline)
    ret.insert("style:text-outline", "true");
  if (style.shadow ^ defaultCharStyle.shadow)
    ret.insert("fo:text-shadow", "1pt 1pt");
  if (style.smallCaps ^ defaultCharStyle.smallCaps)
    ret.insert("fo:font-variant", "small-caps");
  else if (style.allCaps ^ defaultCharStyle.allCaps)
    ret.insert("fo:text-transform", "uppercase");
  if (style.emboss ^ defaultCharStyle.emboss)
    ret.insert("style:font-relief", "embossed");
  else if (style.engrave ^ defaultCharStyle.engrave)
    ret.insert("style:font-relief", "engraved");
  if (style.underline)
    fillUnderline(ret, get(style.underline));
  else if (defaultCharStyle.underline)
    fillUnderline(ret, get(defaultCharStyle.underline));
  if (style.textScale)
    ret.insert("fo:text-scale", get(style.textScale), librevenge::RVNG_PERCENT);
  else if (defaultCharStyle.textScale)
    ret.insert("fo:text-scale", get(defaultCharStyle.textScale), librevenge::RVNG_PERCENT);
  if (bool(style.textSizeInPt))
  {
    ret.insert("fo:font-size", style.textSizeInPt.get() / POINTS_IN_INCH);
  }
  else if (bool(defaultCharStyle.textSizeInPt))
  {
    ret.insert("fo:font-size", defaultCharStyle.textSizeInPt.get()
               / POINTS_IN_INCH);
  }
  if (style.colorIndex >= 0 && (size_t)style.colorIndex < m_textColors.size())
  {
    ret.insert("fo:color", getColorString(m_textColors[style.colorIndex].getFinalColor(m_paletteColors)));
  }
  else if (defaultCharStyle.colorIndex >= 0 && (size_t)defaultCharStyle.colorIndex < m_textColors.size())
  {
    ret.insert("fo:color", getColorString(m_textColors[defaultCharStyle.colorIndex].getFinalColor(m_paletteColors)));
  }
  else
  {
    ret.insert("fo:color", getColorString(Color(0, 0, 0)));  // default color is black
  }
  if (bool(style.fontIndex) &&
      style.fontIndex.get() < m_fonts.size())
  {
    librevenge::RVNGString str;
    appendCharacters(str, m_fonts[style.fontIndex.get()],
                     getCalculatedEncoding());
    ret.insert("style:font-name", str);
  }
  else if (bool(defaultCharStyle.fontIndex) &&
           defaultCharStyle.fontIndex.get() < m_fonts.size())
  {
    librevenge::RVNGString str;
    appendCharacters(str, m_fonts[defaultCharStyle.fontIndex.get()],
                     getCalculatedEncoding());
    ret.insert("style:font-name", str);
  }
  else if (!m_fonts.empty())
  {
    librevenge::RVNGString str;
    appendCharacters(str, m_fonts[0],
                     getCalculatedEncoding());
    ret.insert("style:font-name", str);
  }
  switch (style.superSubType)
  {
  case SUPERSCRIPT:
    ret.insert("style:text-position", "50% 67%");
    break;
  case SUBSCRIPT:
    ret.insert("style:text-position", "-50% 67%");
    break;
  default:
    break;
  }
  if (style.lcid)
    fillLocale(ret, get(style.lcid));
  else if (defaultCharStyle.lcid)
    fillLocale(ret, get(defaultCharStyle.lcid));
  return ret;
}

librevenge::RVNGString MSPUBCollector::getColorString(const Color &color)
{
  librevenge::RVNGString ret;
  ret.sprintf("#%.2x%.2x%.2x",(unsigned char)color.r, (unsigned char)color.g, (unsigned char)color.b);
  MSPUB_DEBUG_MSG(("String for r: 0x%x, g: 0x%x, b: 0x%x is %s\n", color.r, color.g, color.b, ret.cstr()));
  return ret;
}

void MSPUBCollector::addBlackToPaletteIfNecessary()
{
  if (m_paletteColors.size() < 8)
  {
    m_paletteColors.insert(m_paletteColors.begin(), Color());
  }
}

void MSPUBCollector::assignShapesToPages()
{
  for (auto &topLevelShape : m_topLevelShapes)
  {
    unsigned *ptr_pageSeqNum = getIfExists(m_pageSeqNumsByShapeSeqNum, topLevelShape->getSeqNum());
    topLevelShape->setup(std::bind(&MSPUBCollector::setupShapeStructures, this, _1));
    if (ptr_pageSeqNum)
    {
      PageInfo *ptr_page = getIfExists(m_pagesBySeqNum, *ptr_pageSeqNum);
      if (ptr_page)
      {
        ptr_page->m_shapeGroupsOrdered.push_back(topLevelShape);
      }
    }
  }
}

boost::optional<unsigned> MSPUBCollector::getMasterPageSeqNum(unsigned pageSeqNum) const
{
  boost::optional<unsigned> toReturn;
  const unsigned *ptr_masterSeqNum = getIfExists_const(m_masterPagesByPageSeqNum, pageSeqNum);
  if (ptr_masterSeqNum && hasPage(*ptr_masterSeqNum) &&
      m_masterPages.find(*ptr_masterSeqNum) != m_masterPages.end())
  {
    return *ptr_masterSeqNum;
  }
  return toReturn;
}

void MSPUBCollector::writePage(unsigned pageSeqNum, int pageKind) const
{
  const PageInfo &pageInfo = m_pagesBySeqNum.find(pageSeqNum)->second;
  librevenge::RVNGPropertyList pageProps;
  if (m_widthSet)
  {
    pageProps.insert("svg:width", m_width);
  }
  if (m_heightSet)
  {
    pageProps.insert("svg:height", m_height);
  }
  const auto &shapeGroupsOrdered = pageInfo.m_shapeGroupsOrdered;
  // pub-converter: pageKind 0 = as upstream (only pages with shapes of their own),
  // 1 = a real page from the document page list: always written, even when it only
  // shows master content or is blank; 2 = one of the special non-printing pages at
  // the end of the page list (scratch area etc.): never written.
  if (pageKind == 2)
    return;
  // pub-converter: old files (Publisher 97/98/2000, upstream page path) carry extra pages whose shapes all have
  // zero size (template stubs; checked in 7 corpus files: always rectangle/ellipse/line stubs of 0 x 0 in). They
  // came out as a blank first page with the real page 1 second. Such pages are not written.
  if (pageKind == 0)
  {
    bool anyVisible = false, anyShape = false;
    for (const auto &kv : m_shapeInfosBySeqNum)
    {
      if (!kv.second.m_pageSeqNum || kv.second.m_pageSeqNum.get() != pageSeqNum)
        continue;
      anyShape = true;
      const auto &c = kv.second.m_coordinates;
      if (c && (c->getWidthIn() > 0 || c->getHeightIn() > 0))
      {
        anyVisible = true;
        break;
      }
    }
    if (anyShape && !anyVisible)
      return;
  }
  if (!shapeGroupsOrdered.empty() || pageKind == 1)
  {
    boost::optional<unsigned> masterSeqNum = getMasterPageSeqNum(pageSeqNum);
    auto hasMaster = bool(masterSeqNum);
    pageProps.insert("pub:page-id", int(pageSeqNum));
    if (hasMaster)
      pageProps.insert("pub:master-page-id", int(masterSeqNum.get()));
    m_painter->startPage(pageProps);
    if (hasMaster)
    {
      writePageBackground(masterSeqNum.get());
    }
    writePageBackground(pageSeqNum);
    if (hasMaster)
    {
      // pub-converter: master content in its own layer so consumers can tell it apart
      librevenge::RVNGPropertyList masterProps;
      masterProps.insert("pub:master-page-id", int(masterSeqNum.get()));
      m_painter->startLayer(masterProps);
      writePageShapes(masterSeqNum.get());
      m_painter->endLayer();
    }
    writePageShapes(pageSeqNum);
    m_painter->endPage();
  }
}

void MSPUBCollector::writePageShapes(unsigned pageSeqNum) const
{
  const PageInfo &pageInfo = m_pagesBySeqNum.find(pageSeqNum)->second;
  for (const auto &shapeGroup : pageInfo.m_shapeGroupsOrdered)
    shapeGroup->visit(std::bind(&MSPUBCollector::paintShape, this, _1, _2, _3, _4, _5));
}

void MSPUBCollector::writePageBackground(unsigned pageSeqNum) const
{
  const unsigned *ptr_fillSeqNum = getIfExists_const(m_bgShapeSeqNumsByPageSeqNum, pageSeqNum);
  if (ptr_fillSeqNum)
  {
    std::shared_ptr<const Fill> ptr_fill;
    const ShapeInfo *ptr_info = getIfExists_const(m_shapeInfosBySeqNum, *ptr_fillSeqNum);
    if (ptr_info)
    {
      ptr_fill = ptr_info->m_fill;
    }
    if (ptr_fill)
    {
      ShapeInfo bg;
      bg.m_type = RECTANGLE;
      Coordinate wholePage(-m_width/2 * EMUS_IN_INCH, -m_height/2 * EMUS_IN_INCH, m_width/2 * EMUS_IN_INCH, m_height/2 * EMUS_IN_INCH);
      bg.m_coordinates = wholePage;
      bg.m_pageSeqNum = pageSeqNum;
      bg.m_fill = ptr_fill;
      paintShape(bg, Coordinate(), VectorTransformation2D(), false, VectorTransformation2D());
    }
  }
}

bool MSPUBCollector::pageIsMaster(unsigned pageSeqNum) const
{
  return m_masterPages.find(pageSeqNum) != m_masterPages.end();
}

bool MSPUBCollector::go()
{
  addBlackToPaletteIfNecessary();
  assignShapesToPages();
  collectTextChains();
  m_painter->startDocument(librevenge::RVNGPropertyList());
  m_painter->setDocumentMetaData(m_metaData);

  for (std::list<EmbeddedFontInfo>::const_iterator i = m_embeddedFonts.begin(); i != m_embeddedFonts.end(); ++i)
  {
    librevenge::RVNGPropertyList props;
    props.insert("librevenge:name", i->m_name);
    props.insert("librevenge:mime-type", "application/vnd.ms-fontobject");
    props.insert("office:binary-data",i->m_blob);
    m_painter->defineEmbeddedFont(props);
  }

  if (m_pageSeqNumsOrdered.empty())
  {
    for (std::map<unsigned, PageInfo>::const_iterator i = m_pagesBySeqNum.begin();
         i != m_pagesBySeqNum.end(); ++i)
    {
      if (!pageIsMaster(i->first))
      {
        writePage(i->first, 0);
      }
    }
  }
  else
  {
    // pub-converter: in every 2002+ file of our corpus the last 4 entries of the
    // document page list are special pages (scratch area etc.), never printed.
    const size_t firstSpecial = m_pageSeqNumsOrdered.size() > 4 ? m_pageSeqNumsOrdered.size() - 4 : m_pageSeqNumsOrdered.size();
    for (size_t k = 0; k < m_pageSeqNumsOrdered.size(); ++k)
    {
      const unsigned i = m_pageSeqNumsOrdered[k];
      std::map<unsigned, PageInfo>::const_iterator iter =
        m_pagesBySeqNum.find(i);
      if (iter != m_pagesBySeqNum.end() && !pageIsMaster(iter->first))
      {
        writePage(iter->first, k < firstSpecial ? 1 : 2);
      }
    }
  }
  m_painter->endDocument();
  return true;
}


bool MSPUBCollector::addTextString(const std::vector<TextParagraph> &str, unsigned id)
{
  MSPUB_DEBUG_MSG(("addTextString, id: 0x%x\n", id));
  m_textStringsById[id] = str;
  if (m_encodingHeuristic)
  {
    ponderStringEncoding(str);
  }
  return true; //FIXME: Warn if the string already existed in the map.
}

void MSPUBCollector::ponderStringEncoding(
  const std::vector<TextParagraph> &str)
{
  for (const auto &i : str)
  {
    for (size_t j = 0; j < i.spans.size(); ++j)
    {
      const std::vector<unsigned char> &chars = i.spans[j].chars;
      m_allText.insert(m_allText.end(), chars.begin(), chars.end());
    }
  }
}

void MSPUBCollector::setWidthInEmu(unsigned long widthInEmu)
{
  //FIXME: Warn if this is called twice
  m_width = ((double)widthInEmu) / EMUS_IN_INCH;
  m_widthSet = true;
}

void MSPUBCollector::setHeightInEmu(unsigned long heightInEmu)
{
  //FIXME: Warn if this is called twice
  m_height = ((double)heightInEmu) / EMUS_IN_INCH;
  m_heightSet = true;
}

bool MSPUBCollector::addImage(unsigned index, ImgType type, librevenge::RVNGBinaryData img)
{
  while (m_images.size() < index)
  {
    m_images.push_back(std::pair<ImgType, librevenge::RVNGBinaryData>(UNKNOWN, librevenge::RVNGBinaryData()));
  }
  if (index > 0)
  {
    MSPUB_DEBUG_MSG(("Image at index %u and of type 0x%x added.\n", index, type));
    m_images[index - 1] = std::pair<ImgType, librevenge::RVNGBinaryData>(type, img);
  }
  else
  {
    MSPUB_DEBUG_MSG(("0 is not a valid index for image, ignoring.\n"));
  }
  return index > 0;
}

librevenge::RVNGBinaryData *MSPUBCollector::addBorderImage(ImgType type,
                                                           unsigned borderArtIndex)
{
  while (borderArtIndex >= m_borderImages.size())
  {
    m_borderImages.push_back(BorderArtInfo());
  }
  m_borderImages[borderArtIndex].m_images.push_back(BorderImgInfo(type));
  return &(m_borderImages[borderArtIndex].m_images.back().m_imgBlob);
}

void MSPUBCollector::setBorderImageOffset(unsigned index, unsigned offset)
{
  while (index >= m_borderImages.size())
  {
    m_borderImages.push_back(BorderArtInfo());
  }
  BorderArtInfo &bai = m_borderImages[index];
  bai.m_offsets.push_back(offset);
  bool added = false;
  for (auto i = bai.m_offsetsOrdered.begin();
       i != bai.m_offsetsOrdered.end(); ++i)
  {
    if (*i >= offset)
    {
      bai.m_offsetsOrdered.insert(i, offset);
      added = true;
      break;
    }
  }
  if (!added)
  {
    bai.m_offsetsOrdered.push_back(offset);
  }
}

void MSPUBCollector::setShapePage(unsigned seqNum, unsigned pageSeqNum)
{
  m_shapeInfosBySeqNum[seqNum].m_pageSeqNum = pageSeqNum;
  m_pageSeqNumsByShapeSeqNum[seqNum] = pageSeqNum;
}

void MSPUBCollector::addTextColor(ColorReference c)
{
  m_textColors.push_back(c);
}

void MSPUBCollector::designateMasterPage(unsigned seqNum)
{
  m_masterPages.insert(seqNum);
}

void MSPUBCollector::setMasterPage(unsigned seqNum, unsigned masterPageSeqNum)
{
  m_masterPagesByPageSeqNum[seqNum] = masterPageSeqNum;
}

void MSPUBCollector::setShapeCropType(unsigned seqNum, unsigned cropType)
{
  m_shapeInfosBySeqNum[seqNum].m_cropType = cropType;
}

}

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
