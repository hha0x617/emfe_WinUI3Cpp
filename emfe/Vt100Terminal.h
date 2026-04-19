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

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

/// Minimal VT100/ANSI terminal emulator with character cell screen buffer.
/// Supports cursor movement, screen/line clearing, scrolling regions,
/// and line-drawing characters -- sufficient for curses-based applications
/// like NetBSD sysinst.
class Vt100Terminal {
public:
    explicit Vt100Terminal(int cols = 80, int rows = 25, int maxScrollback = 2000);

    int GetCols() const { return m_cols; }
    int GetRows() const { return m_rows; }

    bool IsDirty() const { return m_dirty; }
    void ClearDirty() { m_dirty = false; }
    void SetDirty() { m_dirty = true; }

    // Write a single character through the terminal emulator.
    void Write(char ch);

    // Write a string through the terminal emulator.
    void Write(const std::string& s);

    /// Render the screen buffer as a single string with newline-separated rows.
    /// Trailing spaces on each row are preserved to maintain column alignment.
    std::string Render() const;

    /// Render the screen buffer with a block cursor at the current cursor position.
    std::string RenderWithCursor() const;

    /// Render the full terminal output: scrollback history followed by the live screen.
    std::string RenderFull() const;

    /// Render the full terminal output with a block cursor at the current position.
    std::string RenderFullWithCursor() const;

    /// Resize the scrollback ring buffer, preserving the most recent lines.
    void ResizeScrollback(int newMax);

    /// Resize the terminal screen to new dimensions, preserving existing content.
    void Resize(int newCols, int newRows);

    int GetScrollbackLineCount() const { return m_scrollbackCount; }
    int GetCursorRow() const { return m_cursorRow; }
    int GetCursorCol() const { return m_cursorCol; }

    /// DEC Cursor Key Mode (DECCKM): when true, arrow keys send ESC O x instead of ESC [ x
    bool GetApplicationCursorKeys() const { return m_applicationCursorKeys; }

private:
    // Parser state machine
    enum class State { Normal, Esc, Csi, EscParen, StringSeq };

    // Screen dimensions
    int m_cols;
    int m_rows;

    // Screen buffer: row-major, index = row * m_cols + col
    std::vector<char> m_screen;

    // Soft-wrap flags: true if this row is a continuation of the previous row
    // (auto-wrap at column limit, not a real newline from the guest).
    std::vector<bool> m_softWrap;

    // Cursor position
    int m_cursorRow = 0;
    int m_cursorCol = 0;

    bool m_dirty = true;

    // Scrollback ring buffer
    std::vector<std::string> m_scrollback;
    std::vector<bool> m_scrollbackSoftWrap; // true if line is a soft-wrap continuation
    int m_scrollbackHead = 0;  // Index of oldest line
    int m_scrollbackCount = 0; // Number of lines stored
    int m_maxScrollback;

    // Parser state
    State m_state = State::Normal;
    std::vector<int> m_csiParams;
    int m_currentParam = 0;
    bool m_hasCurrentParam = false;
    bool m_csiQuestion = false; // CSI ? prefix

    // Scroll region (0-based, inclusive)
    int m_scrollTop = 0;
    int m_scrollBottom = 0;

    // Saved cursor position (DECSC / DECRC)
    int m_savedRow = 0;
    int m_savedCol = 0;

    // DEC Cursor Key Mode
    bool m_applicationCursorKeys = false;

    // Alternate character set (line drawing)
    bool m_alternateCharset = false;

    // VT100 line drawing characters mapped from ASCII
    static const std::unordered_map<char, char32_t> s_lineDrawingMap;

    // Helper to encode a Unicode code point as a UTF-8 sequence and append to a string
    static void AppendUtf8(std::string& out, char32_t codepoint);

    // Normal character processing
    void ProcessNormal(char ch);
    void PutChar(char ch);
    void LineFeed();

    // ESC sequence processing
    void ProcessEsc(char ch);

    // CSI sequence processing
    void ProcessCsi(char ch);
    int Param(int index, int defaultValue = 1) const;
    void ExecuteCsi(char cmd);

    // OSC / DCS / PM / APC string sequence processing (consume until ST or BEL)
    bool m_stringSeqEsc = false; // true when ESC seen inside string sequence
    void ProcessStringSeq(char ch);

    // Erase operations
    void EraseInDisplay(int mode);
    void EraseInLine(int mode);
    void EraseChars(int n);

    // Insert/Delete operations
    void InsertLines(int n);
    void DeleteLines(int n);
    void DeleteChars(int n);
    void InsertChars(int n);

    // Scrolling
    void ScrollUp(int n);
    void ScrollDown(int n);

    // Helpers
    void ClearScreen();
    void ClearRange(int r1, int c1, int r2, int c2);
    void Reset();

    // Screen cell access helpers
    char& ScreenAt(int row, int col) { return m_screen[row * m_cols + col]; }
    char ScreenAt(int row, int col) const { return m_screen[row * m_cols + col]; }
};
