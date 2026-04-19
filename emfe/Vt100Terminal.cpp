// Copyright 2026 hha0x617
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Vt100Terminal.h"

#include <algorithm>
#include <sstream>

// ============================================================================
// VT100 line drawing map (ASCII -> Unicode code point)
// ============================================================================

const std::unordered_map<char, char32_t> Vt100Terminal::s_lineDrawingMap = {
    {'j', U'\u2518'}, // box drawings light up and left
    {'k', U'\u2510'}, // box drawings light down and left
    {'l', U'\u250C'}, // box drawings light down and right
    {'m', U'\u2514'}, // box drawings light up and right
    {'n', U'\u253C'}, // box drawings light vertical and horizontal
    {'q', U'\u2500'}, // box drawings light horizontal
    {'t', U'\u251C'}, // box drawings light vertical and right
    {'u', U'\u2524'}, // box drawings light vertical and left
    {'v', U'\u2534'}, // box drawings light up and horizontal
    {'w', U'\u252C'}, // box drawings light down and horizontal
    {'x', U'\u2502'}, // box drawings light vertical
    {'a', U'\u2592'}, // medium shade
    {'f', U'\u00B0'}, // degree sign
    {'g', U'\u00B1'}, // plus-minus sign
    {'~', U'\u00B7'}, // middle dot
    {'y', U'\u2264'}, // less-than or equal to
    {'z', U'\u2265'}, // greater-than or equal to
};

// ============================================================================
// UTF-8 encoding helper
// ============================================================================

void Vt100Terminal::AppendUtf8(std::string& out, char32_t cp)
{
    if (cp <= 0x7F)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0x10FFFF)
    {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ============================================================================
// Constructor
// ============================================================================

Vt100Terminal::Vt100Terminal(int cols, int rows, int maxScrollback)
    : m_cols(cols)
    , m_rows(rows)
    , m_maxScrollback(std::clamp(maxScrollback, 0, 100000))
    , m_scrollBottom(rows - 1)
{
    m_scrollback.resize(m_maxScrollback > 0 ? m_maxScrollback : 1);
    m_scrollbackSoftWrap.resize(m_scrollback.size(), false);
    m_screen.resize(rows * cols, ' ');
    m_softWrap.resize(rows, false);
}

// ============================================================================
// ResizeScrollback
// ============================================================================

void Vt100Terminal::ResizeScrollback(int newMax)
{
    newMax = std::clamp(newMax, 0, 100000);
    if (newMax == m_maxScrollback) return;

    std::vector<std::string> newBuf(newMax > 0 ? newMax : 1);
    std::vector<bool> newWrap(newBuf.size(), false);
    if (newMax > 0 && m_scrollbackCount > 0)
    {
        int copyCount = std::min(m_scrollbackCount, newMax);
        int srcStart = (m_scrollbackHead + m_scrollbackCount - copyCount) % m_maxScrollback;
        for (int i = 0; i < copyCount; i++)
        {
            int srcIdx = (srcStart + i) % m_maxScrollback;
            newBuf[i] = std::move(m_scrollback[srcIdx]);
            newWrap[i] = m_scrollbackSoftWrap[srcIdx];
        }
        m_scrollbackHead = 0;
        m_scrollbackCount = copyCount;
    }
    else
    {
        m_scrollbackHead = 0;
        m_scrollbackCount = 0;
    }

    m_scrollback = std::move(newBuf);
    m_scrollbackSoftWrap = std::move(newWrap);
    m_maxScrollback = newMax;
    m_dirty = true;
}

// ============================================================================
// Resize
// ============================================================================

void Vt100Terminal::Resize(int newCols, int newRows)
{
    if (newCols == m_cols && newRows == m_rows) return;

    int rowDiff = newRows - m_rows; // positive = growing, negative = shrinking
    std::vector<char> newScreen(newRows * newCols, ' ');

    if (rowDiff < 0)
    {
        // --- Shrinking height ---
        int rowsToRemove = -rowDiff;

        // Count trailing blank lines at bottom of screen (up to rowsToRemove)
        int trailingBlanks = 0;
        for (int r = m_rows - 1; r >= 0 && trailingBlanks < rowsToRemove; r--)
        {
            bool blank = true;
            for (int c = 0; c < m_cols; c++)
            {
                if (ScreenAt(r, c) != ' ') { blank = false; break; }
            }
            if (!blank) break;
            trailingBlanks++;
        }

        // Don't remove blank lines at or above cursor (cursor row must remain visible)
        int removableBlanks = std::min(trailingBlanks, m_rows - 1 - m_cursorRow);
        int blanksConsumed = std::min(removableBlanks, rowsToRemove);
        int remainingToRemove = rowsToRemove - blanksConsumed;

        // Push top lines to scrollback (anchor bottom of display)
        if (remainingToRemove > 0 && m_maxScrollback > 0)
        {
            for (int r = 0; r < remainingToRemove; r++)
            {
                std::string line(m_cols, ' ');
                for (int c = 0; c < m_cols; c++)
                    line[c] = ScreenAt(r, c);
                auto end = line.find_last_not_of(' ');
                if (end != std::string::npos)
                    line.resize(end + 1);
                else
                    line.clear();

                int writeIdx = (m_scrollbackHead + m_scrollbackCount) % m_maxScrollback;
                m_scrollback[writeIdx] = std::move(line);
                if (m_scrollbackCount < m_maxScrollback)
                    m_scrollbackCount++;
                else
                    m_scrollbackHead = (m_scrollbackHead + 1) % m_maxScrollback;
            }
        }

        // Copy: skip top `remainingToRemove` lines, drop bottom `blanksConsumed` blank lines
        int srcStartRow = remainingToRemove;
        int copyRows = std::min(m_rows - remainingToRemove - blanksConsumed, newRows);
        int copyCols = std::min(m_cols, newCols);
        for (int r = 0; r < copyRows; r++)
            for (int c = 0; c < copyCols; c++)
                newScreen[r * newCols + c] = ScreenAt(srcStartRow + r, c);

        m_cursorRow = std::clamp(m_cursorRow - remainingToRemove, 0, newRows - 1);
    }
    else if (rowDiff > 0)
    {
        // --- Growing height ---
        int rowsToAdd = rowDiff;

        // Pull most recent lines from scrollback to fill top
        int fromScrollback = std::min(rowsToAdd, m_scrollbackCount);

        for (int i = 0; i < fromScrollback; i++)
        {
            // Older lines at top, newer lines adjacent to existing content
            int scrollIdx = (m_scrollbackHead + m_scrollbackCount - fromScrollback + i) % m_maxScrollback;
            const std::string& line = m_scrollback[scrollIdx];
            int len = std::min(static_cast<int>(line.size()), newCols);
            for (int c = 0; c < len; c++)
                newScreen[i * newCols + c] = line[c];
        }
        m_scrollbackCount -= fromScrollback;

        // Copy existing screen content after the restored scrollback lines
        int destStartRow = fromScrollback;
        int copyRows = std::min(m_rows, newRows - fromScrollback);
        int copyCols = std::min(m_cols, newCols);
        for (int r = 0; r < copyRows; r++)
            for (int c = 0; c < copyCols; c++)
                newScreen[(destStartRow + r) * newCols + c] = ScreenAt(r, c);

        // Remaining rows at bottom are already blank (initialized to ' ')
        m_cursorRow = std::clamp(m_cursorRow + fromScrollback, 0, newRows - 1);
    }
    else
    {
        // Only column width changed
        int copyCols = std::min(m_cols, newCols);
        for (int r = 0; r < m_rows; r++)
            for (int c = 0; c < copyCols; c++)
                newScreen[r * newCols + c] = ScreenAt(r, c);
    }

    m_screen = std::move(newScreen);
    m_cursorCol = std::clamp(m_cursorCol, 0, newCols - 1);
    m_cols = newCols;
    m_rows = newRows;
    m_scrollTop = 0;
    m_scrollBottom = newRows - 1;
    m_dirty = true;
}

// ============================================================================
// Write
// ============================================================================

void Vt100Terminal::Write(char ch)
{
    switch (m_state)
    {
        case State::Normal:
            ProcessNormal(ch);
            break;
        case State::Esc:
            ProcessEsc(ch);
            break;
        case State::Csi:
            ProcessCsi(ch);
            break;
        case State::EscParen:
            // ESC ( or ESC ) -- character set designation, consume one more char
            m_state = State::Normal;
            break;
        case State::StringSeq:
            ProcessStringSeq(ch);
            break;
    }
}

void Vt100Terminal::Write(const std::string& s)
{
    for (char ch : s)
        Write(ch);
}

// ============================================================================
// Render
// ============================================================================

std::string Vt100Terminal::Render() const
{
    std::string result;
    result.reserve(m_rows * (m_cols * 3 + 1)); // Up to 3 bytes per char (UTF-8) + newlines
    for (int r = 0; r < m_rows; r++)
    {
        // Insert newline before this row, unless it's a soft-wrap continuation
        if (r > 0 && !m_softWrap[r]) result.push_back('\n');

        // Trim trailing spaces, but keep cursor row consistent with RenderWithCursor
        int lastCol = m_cols - 1;
        while (lastCol >= 0 && ScreenAt(r, lastCol) == ' ') lastCol--;
        if (r == m_cursorRow && m_cursorCol > lastCol) lastCol = m_cursorCol;
        for (int c = 0; c <= lastCol; c++)
        {
            char ch = ScreenAt(r, c);
            // Use non-breaking space at cursor position to prevent TextBox
            // from collapsing trailing spaces at wrap boundaries (jitter fix)
            if (r == m_cursorRow && c == m_cursorCol && ch == ' ')
                AppendUtf8(result, U'\u00A0');
            else
                result.push_back(ch);
        }
    }
    return result;
}

// ============================================================================
// RenderWithCursor
// ============================================================================

std::string Vt100Terminal::RenderWithCursor() const
{
    std::string result;
    result.reserve(m_rows * (m_cols * 3 + 1));
    for (int r = 0; r < m_rows; r++)
    {
        if (r > 0 && !m_softWrap[r]) result.push_back('\n');

        // Trim trailing spaces, but ensure cursor column is included
        int lastCol = m_cols - 1;
        while (lastCol >= 0 && ScreenAt(r, lastCol) == ' ') lastCol--;
        if (r == m_cursorRow && m_cursorCol > lastCol) lastCol = m_cursorCol;

        for (int c = 0; c <= lastCol; c++)
        {
            if (r == m_cursorRow && c == m_cursorCol)
            {
                AppendUtf8(result, U'\u2588');
            }
            else
            {
                result.push_back(ScreenAt(r, c));
            }
        }
    }
    return result;
}

// ============================================================================
// RenderFull
// ============================================================================

std::string Vt100Terminal::RenderFull() const
{
    std::string result;
    result.reserve(m_scrollbackCount * (m_cols + 1) + m_rows * (m_cols + 1));

    for (int i = 0; i < m_scrollbackCount; i++)
    {
        int idx = (m_scrollbackHead + i) % m_maxScrollback;
        if (i > 0 && !m_scrollbackSoftWrap[idx])
            result.push_back('\n');
        result.append(m_scrollback[idx]);
    }

    for (int r = 0; r < m_rows; r++)
    {
        if (m_scrollbackCount > 0 || r > 0)
        {
            if (!m_softWrap[r])
                result.push_back('\n');
        }
        int lastCol = m_cols - 1;
        while (lastCol >= 0 && ScreenAt(r, lastCol) == ' ') lastCol--;
        if (r == m_cursorRow && m_cursorCol > lastCol) lastCol = m_cursorCol;
        for (int c = 0; c <= lastCol; c++)
        {
            char ch = ScreenAt(r, c);
            if (r == m_cursorRow && c == m_cursorCol && ch == ' ')
                AppendUtf8(result, U'\u00A0');
            else
                result.push_back(ch);
        }
    }
    return result;
}

// ============================================================================
// RenderFullWithCursor
// ============================================================================

std::string Vt100Terminal::RenderFullWithCursor() const
{
    std::string result;
    result.reserve(m_scrollbackCount * (m_cols + 1) + m_rows * (m_cols * 3 + 1));

    for (int i = 0; i < m_scrollbackCount; i++)
    {
        int idx = (m_scrollbackHead + i) % m_maxScrollback;
        if (i > 0 && !m_scrollbackSoftWrap[idx])
            result.push_back('\n');
        result.append(m_scrollback[idx]);
    }

    for (int r = 0; r < m_rows; r++)
    {
        if (m_scrollbackCount > 0 || r > 0)
        {
            if (!m_softWrap[r])
                result.push_back('\n');
        }
        int lastCol = m_cols - 1;
        while (lastCol >= 0 && ScreenAt(r, lastCol) == ' ') lastCol--;
        if (r == m_cursorRow && m_cursorCol > lastCol) lastCol = m_cursorCol;
        for (int c = 0; c <= lastCol; c++)
        {
            if (r == m_cursorRow && c == m_cursorCol)
                AppendUtf8(result, U'\u2588');
            else
                result.push_back(ScreenAt(r, c));
        }
    }
    return result;
}

// ============================================================================
// Normal character processing
// ============================================================================

void Vt100Terminal::ProcessNormal(char ch)
{
    switch (ch)
    {
        case '\x1B': // ESC
            m_state = State::Esc;
            break;
        case '\r': // CR
            m_cursorCol = 0;
            m_dirty = true;
            break;
        case '\n': // LF -- also do CR (the write bypass skips tty ONLCR processing)
            m_cursorCol = 0;
            LineFeed();
            m_softWrap[m_cursorRow] = false; // Real newline, not a continuation
            break;
        case '\b': // BS
            if (m_cursorCol > 0) m_cursorCol--;
            m_dirty = true;
            break;
        case '\t': // TAB
            m_cursorCol = std::min((m_cursorCol / 8 + 1) * 8, m_cols - 1);
            m_dirty = true;
            break;
        case '\x07': // BEL
            break;
        case '\x0E': // SO -- Switch to alternate character set
            m_alternateCharset = true;
            break;
        case '\x0F': // SI -- Switch to standard character set
            m_alternateCharset = false;
            break;
        default:
            if (ch >= ' ')
                PutChar(ch);
            break;
    }
}

void Vt100Terminal::PutChar(char ch)
{
    // Map line-drawing characters when in alternate charset
    // Since we store char (not wchar_t), line-drawing Unicode chars cannot be
    // stored directly in the screen buffer. We store a placeholder '?' for
    // line drawing in the cell and handle rendering separately.
    // However, for simplicity and faithful port: we keep the char buffer and
    // let the Render methods output plain chars. If you need full Unicode
    // line drawing in the screen buffer, switch m_screen to std::vector<char32_t>.
    if (m_alternateCharset)
    {
        auto it = s_lineDrawingMap.find(ch);
        if (it != s_lineDrawingMap.end())
        {
            // Store a substitute ASCII approximation in the cell
            // (Full Unicode rendering is handled at the UI layer)
            switch (ch)
            {
                case 'j': ch = '+'; break; // corner
                case 'k': ch = '+'; break; // corner
                case 'l': ch = '+'; break; // corner
                case 'm': ch = '+'; break; // corner
                case 'n': ch = '+'; break; // cross
                case 'q': ch = '-'; break; // horizontal
                case 't': ch = '+'; break; // tee
                case 'u': ch = '+'; break; // tee
                case 'v': ch = '+'; break; // tee
                case 'w': ch = '+'; break; // tee
                case 'x': ch = '|'; break; // vertical
                case 'a': ch = '#'; break; // shade
                case 'f': ch = 'o'; break; // degree
                case 'g': ch = '+'; break; // plus-minus
                case '~': ch = '.'; break; // middle dot
                case 'y': ch = '<'; break; // less-equal
                case 'z': ch = '>'; break; // greater-equal
                default: break;
            }
        }
    }

    if (m_cursorCol >= m_cols)
    {
        // Auto-wrap: mark the next line as a soft-wrap continuation
        m_cursorCol = 0;
        LineFeed();
        m_softWrap[m_cursorRow] = true;
    }

    ScreenAt(m_cursorRow, m_cursorCol) = ch;
    m_cursorCol++;
    m_dirty = true;
}

void Vt100Terminal::LineFeed()
{
    if (m_cursorRow == m_scrollBottom)
        ScrollUp(1);
    else if (m_cursorRow < m_rows - 1)
        m_cursorRow++;
    m_dirty = true;
}

// ============================================================================
// ESC sequence processing
// ============================================================================

void Vt100Terminal::ProcessEsc(char ch)
{
    switch (ch)
    {
        case '[': // CSI
            m_state = State::Csi;
            m_csiParams.clear();
            m_currentParam = 0;
            m_hasCurrentParam = false;
            m_csiQuestion = false;
            break;
        case '(':
        case ')': // Character set designation
            m_state = State::EscParen;
            break;
        case '7': // DECSC -- Save cursor
            m_savedRow = m_cursorRow;
            m_savedCol = m_cursorCol;
            m_state = State::Normal;
            break;
        case '8': // DECRC -- Restore cursor
            m_cursorRow = std::clamp(m_savedRow, 0, m_rows - 1);
            m_cursorCol = std::clamp(m_savedCol, 0, m_cols - 1);
            m_dirty = true;
            m_state = State::Normal;
            break;
        case 'M': // RI -- Reverse Index (cursor up, scroll down if at top)
            if (m_cursorRow == m_scrollTop)
                ScrollDown(1);
            else if (m_cursorRow > 0)
                m_cursorRow--;
            m_dirty = true;
            m_state = State::Normal;
            break;
        case 'D': // IND -- Index (cursor down, scroll up if at bottom)
            LineFeed();
            m_state = State::Normal;
            break;
        case 'E': // NEL -- Next Line
            m_cursorCol = 0;
            LineFeed();
            m_state = State::Normal;
            break;
        case ']': // OSC -- Operating System Command
        case 'P': // DCS -- Device Control String
        case '^': // PM  -- Privacy Message
        case '_': // APC -- Application Program Command
            m_state = State::StringSeq;
            m_stringSeqEsc = false;
            break;
        case '=':
        case '>': // Keypad modes -- ignore
            m_state = State::Normal;
            break;
        case 'c': // RIS -- Full reset
            Reset();
            m_state = State::Normal;
            break;
        default:
            // Unknown ESC sequence -- ignore
            m_state = State::Normal;
            break;
    }
}

// ============================================================================
// OSC / DCS / PM / APC string sequence processing
// Consume all characters until ST (ESC \) or BEL (0x07).
// ============================================================================

void Vt100Terminal::ProcessStringSeq(char ch)
{
    if (m_stringSeqEsc) {
        // Previous character was ESC inside the string sequence
        if (ch == '\\') {
            // ESC \ = ST (String Terminator) -- end of sequence
            m_state = State::Normal;
        } else {
            // ESC followed by something else -- treat as new ESC sequence
            m_state = State::Esc;
            ProcessEsc(ch);
        }
        m_stringSeqEsc = false;
        return;
    }

    if (ch == '\x1B') {
        // ESC inside string sequence -- might be start of ST (ESC \)
        m_stringSeqEsc = true;
    } else if (ch == '\x07') {
        // BEL terminates OSC sequences (xterm extension, widely used)
        m_state = State::Normal;
    } else if (ch == '\x9C') {
        // 8-bit ST (C1 control character)
        m_state = State::Normal;
    }
    // All other characters are consumed silently
}

// ============================================================================
// CSI sequence processing
// ============================================================================

void Vt100Terminal::ProcessCsi(char ch)
{
    if (ch == '?')
    {
        m_csiQuestion = true;
        return;
    }

    if (ch >= '0' && ch <= '9')
    {
        m_currentParam = m_currentParam * 10 + (ch - '0');
        m_hasCurrentParam = true;
        return;
    }

    if (ch == ';' || ch == ':')
    {
        // Semicolon separates parameters; colon separates sub-parameters
        // (e.g., SGR 38:5:185 for 256-color). Treat colon like semicolon
        // since we don't use sub-parameter semantics.
        m_csiParams.push_back(m_hasCurrentParam ? m_currentParam : 0);
        m_currentParam = 0;
        m_hasCurrentParam = false;
        return;
    }

    // Final character -- execute the CSI command
    if (m_hasCurrentParam)
        m_csiParams.push_back(m_currentParam);

    ExecuteCsi(ch);
    m_state = State::Normal;
}

int Vt100Terminal::Param(int index, int defaultValue) const
{
    if (index < static_cast<int>(m_csiParams.size()) && m_csiParams[index] > 0)
        return m_csiParams[index];
    return defaultValue;
}

void Vt100Terminal::ExecuteCsi(char cmd)
{
    if (m_csiQuestion)
    {
        // DEC private modes
        int mode = Param(0);
        if (cmd == 'h') // Set mode
        {
            if (mode == 1) m_applicationCursorKeys = true; // DECCKM
        }
        else if (cmd == 'l') // Reset mode
        {
            if (mode == 1) m_applicationCursorKeys = false; // DECCKM
        }
        // CSI ? 25 h/l = show/hide cursor (ignore)
        // CSI ? 1049 h/l = alternate screen buffer (ignore)
        return;
    }

    switch (cmd)
    {
        case 'A': // CUU -- Cursor Up
            m_cursorRow = std::max(m_cursorRow - Param(0), 0);
            m_dirty = true;
            break;

        case 'B': // CUD -- Cursor Down
            m_cursorRow = std::min(m_cursorRow + Param(0), m_rows - 1);
            m_dirty = true;
            break;

        case 'C': // CUF -- Cursor Forward (Right)
            m_cursorCol = std::min(m_cursorCol + Param(0), m_cols - 1);
            m_dirty = true;
            break;

        case 'D': // CUB -- Cursor Backward (Left)
            m_cursorCol = std::max(m_cursorCol - Param(0), 0);
            m_dirty = true;
            break;

        case 'H': // CUP -- Cursor Position (1-based)
        case 'f':
            m_cursorRow = std::clamp(Param(0) - 1, 0, m_rows - 1);
            m_cursorCol = std::clamp(Param(1, 1) - 1, 0, m_cols - 1);
            m_dirty = true;
            break;

        case 'G': // CHA -- Cursor Horizontal Absolute (1-based)
            m_cursorCol = std::clamp(Param(0) - 1, 0, m_cols - 1);
            m_dirty = true;
            break;

        case 'd': // VPA -- Cursor Vertical Absolute (1-based)
            m_cursorRow = std::clamp(Param(0) - 1, 0, m_rows - 1);
            m_dirty = true;
            break;

        case 'J': // ED -- Erase in Display
            EraseInDisplay(Param(0, 0));
            break;

        case 'K': // EL -- Erase in Line
            EraseInLine(Param(0, 0));
            break;

        case 'L': // IL -- Insert Lines
            InsertLines(Param(0));
            break;

        case 'M': // DL -- Delete Lines
            DeleteLines(Param(0));
            break;

        case 'P': // DCH -- Delete Characters
            DeleteChars(Param(0));
            break;

        case '@': // ICH -- Insert Blank Characters
            InsertChars(Param(0));
            break;

        case 'X': // ECH -- Erase Characters
            EraseChars(Param(0));
            break;

        case 'r': // DECSTBM -- Set Scrolling Region (1-based)
            m_scrollTop = std::clamp(Param(0) - 1, 0, m_rows - 1);
            m_scrollBottom = std::clamp(Param(1, m_rows) - 1, 0, m_rows - 1);
            if (m_scrollTop > m_scrollBottom)
                std::swap(m_scrollTop, m_scrollBottom);
            m_cursorRow = 0;
            m_cursorCol = 0;
            m_dirty = true;
            break;

        case 'S': // SU -- Scroll Up
            ScrollUp(Param(0));
            break;

        case 'T': // SD -- Scroll Down
            ScrollDown(Param(0));
            break;

        case 'm': // SGR -- Select Graphic Rendition (colors/bold -- ignore for now)
            break;

        case 'h':
        case 'l': // SM/RM -- Set/Reset Mode (ignore)
            break;

        case 'n': // DSR -- Device Status Report (ignore)
            break;

        case 's': // SCP -- Save Cursor Position
            m_savedRow = m_cursorRow;
            m_savedCol = m_cursorCol;
            break;

        case 'u': // RCP -- Restore Cursor Position
            m_cursorRow = std::clamp(m_savedRow, 0, m_rows - 1);
            m_cursorCol = std::clamp(m_savedCol, 0, m_cols - 1);
            m_dirty = true;
            break;
    }
}

// ============================================================================
// Erase operations
// ============================================================================

void Vt100Terminal::EraseInDisplay(int mode)
{
    switch (mode)
    {
        case 0: // Erase from cursor to end of screen
            ClearRange(m_cursorRow, m_cursorCol, m_rows - 1, m_cols - 1);
            break;
        case 1: // Erase from start of screen to cursor
            ClearRange(0, 0, m_cursorRow, m_cursorCol);
            break;
        case 2: // Erase entire screen
            ClearScreen();
            break;
    }
    m_dirty = true;
}

void Vt100Terminal::EraseInLine(int mode)
{
    switch (mode)
    {
        case 0: // Erase from cursor to end of line
            for (int c = m_cursorCol; c < m_cols; c++)
                ScreenAt(m_cursorRow, c) = ' ';
            break;
        case 1: // Erase from start of line to cursor
            for (int c = 0; c <= m_cursorCol && c < m_cols; c++)
                ScreenAt(m_cursorRow, c) = ' ';
            break;
        case 2: // Erase entire line
            for (int c = 0; c < m_cols; c++)
                ScreenAt(m_cursorRow, c) = ' ';
            break;
    }
    m_dirty = true;
}

void Vt100Terminal::EraseChars(int n)
{
    for (int i = 0; i < n && m_cursorCol + i < m_cols; i++)
        ScreenAt(m_cursorRow, m_cursorCol + i) = ' ';
    m_dirty = true;
}

// ============================================================================
// Insert/Delete operations
// ============================================================================

void Vt100Terminal::InsertLines(int n)
{
    int bottom = m_scrollBottom;
    for (int i = 0; i < n; i++)
    {
        // Shift lines down within scroll region
        for (int r = bottom; r > m_cursorRow; r--)
            for (int c = 0; c < m_cols; c++)
                ScreenAt(r, c) = ScreenAt(r - 1, c);
        // Clear the inserted line
        for (int c = 0; c < m_cols; c++)
            ScreenAt(m_cursorRow, c) = ' ';
    }
    m_dirty = true;
}

void Vt100Terminal::DeleteLines(int n)
{
    int bottom = m_scrollBottom;
    for (int i = 0; i < n; i++)
    {
        // Shift lines up within scroll region
        for (int r = m_cursorRow; r < bottom; r++)
            for (int c = 0; c < m_cols; c++)
                ScreenAt(r, c) = ScreenAt(r + 1, c);
        // Clear the bottom line
        for (int c = 0; c < m_cols; c++)
            ScreenAt(bottom, c) = ' ';
    }
    m_dirty = true;
}

void Vt100Terminal::DeleteChars(int n)
{
    for (int i = m_cursorCol; i < m_cols; i++)
    {
        int src = i + n;
        ScreenAt(m_cursorRow, i) = src < m_cols ? ScreenAt(m_cursorRow, src) : ' ';
    }
    m_dirty = true;
}

void Vt100Terminal::InsertChars(int n)
{
    for (int i = m_cols - 1; i >= m_cursorCol + n; i--)
        ScreenAt(m_cursorRow, i) = ScreenAt(m_cursorRow, i - n);
    for (int i = 0; i < n && m_cursorCol + i < m_cols; i++)
        ScreenAt(m_cursorRow, m_cursorCol + i) = ' ';
    m_dirty = true;
}

// ============================================================================
// Scrolling
// ============================================================================

void Vt100Terminal::ScrollUp(int n)
{
    for (int i = 0; i < n; i++)
    {
        // Save the top line to scrollback ring buffer before it's overwritten
        if (m_scrollTop == 0 && m_maxScrollback > 0)
        {
            std::string line(m_cols, ' ');
            for (int c = 0; c < m_cols; c++)
                line[c] = ScreenAt(m_scrollTop, c);

            // Trim trailing spaces
            auto end = line.find_last_not_of(' ');
            if (end != std::string::npos)
                line.erase(end + 1);
            else
                line.clear();

            int writeIdx = (m_scrollbackHead + m_scrollbackCount) % m_maxScrollback;
            m_scrollback[writeIdx] = std::move(line);
            m_scrollbackSoftWrap[writeIdx] = m_softWrap[m_scrollTop];
            if (m_scrollbackCount < m_maxScrollback)
                m_scrollbackCount++;
            else
                m_scrollbackHead = (m_scrollbackHead + 1) % m_maxScrollback; // Overwrite oldest
        }

        // Shift soft-wrap flags along with screen rows
        for (int r = m_scrollTop; r < m_scrollBottom; r++)
        {
            for (int c = 0; c < m_cols; c++)
                ScreenAt(r, c) = ScreenAt(r + 1, c);
            m_softWrap[r] = m_softWrap[r + 1];
        }
        for (int c = 0; c < m_cols; c++)
            ScreenAt(m_scrollBottom, c) = ' ';
        m_softWrap[m_scrollBottom] = false;
    }
    m_dirty = true;
}

void Vt100Terminal::ScrollDown(int n)
{
    for (int i = 0; i < n; i++)
    {
        for (int r = m_scrollBottom; r > m_scrollTop; r--)
        {
            for (int c = 0; c < m_cols; c++)
                ScreenAt(r, c) = ScreenAt(r - 1, c);
            m_softWrap[r] = m_softWrap[r - 1];
        }
        for (int c = 0; c < m_cols; c++)
            ScreenAt(m_scrollTop, c) = ' ';
        m_softWrap[m_scrollTop] = false;
    }
    m_dirty = true;
}

// ============================================================================
// Helpers
// ============================================================================

void Vt100Terminal::ClearScreen()
{
    std::fill(m_screen.begin(), m_screen.end(), ' ');
    std::fill(m_softWrap.begin(), m_softWrap.end(), false);
    m_dirty = true;
}

void Vt100Terminal::ClearRange(int r1, int c1, int r2, int c2)
{
    for (int r = r1; r <= r2 && r < m_rows; r++)
    {
        int startC = (r == r1) ? c1 : 0;
        int endC = (r == r2) ? c2 : m_cols - 1;
        for (int c = startC; c <= endC && c < m_cols; c++)
            ScreenAt(r, c) = ' ';
    }
}

void Vt100Terminal::Reset()
{
    ClearScreen();
    m_cursorRow = 0;
    m_cursorCol = 0;
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
    m_alternateCharset = false;
    m_state = State::Normal;
    m_dirty = true;
}
