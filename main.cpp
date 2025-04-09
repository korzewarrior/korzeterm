// Explicitly include required Qt headers
#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QPainter>
#include <QFontDatabase>
#include <QTimer>
#include <QDebug>
#include <QRegularExpression>
#include <QTextCodec>

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pty.h>  // For forkpty
#include <errno.h>

// Terminal character struct to store character attributes
struct TermChar {
    QChar character;
    QColor foreground;
    QColor background;
    bool bold;
    bool italic;
    bool underline;
    
    TermChar() : character(' '), 
                foreground(QColor(235, 219, 178)), 
                background(QColor(40, 40, 40)),
                bold(false),
                italic(false),
                underline(false) {}
};

// Terminal widget class
class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr) : QWidget(parent), m_childPid(-1), m_masterFd(-1) {
        // Set up appearance
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFocusPolicy(Qt::StrongFocus);
        
        // Use monospace font
        m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        m_font.setPointSize(10);
        
        // Font metrics for character dimensions
        m_fontMetrics = new QFontMetrics(m_font);
        m_charWidth = m_fontMetrics->horizontalAdvance('M');
        m_charHeight = m_fontMetrics->height();
        
        // Initialize terminal buffer
        m_rows = 24;
        m_cols = 80;
        m_buffer.resize(m_rows);
        for (int i = 0; i < m_rows; i++) {
            m_buffer[i].resize(m_cols);
        }
        
        // Initialize color palette (xterm-256)
        initializeColorPalette();
        
        // Default colors
        m_defaultFg = QColor(235, 219, 178);
        m_defaultBg = QColor(40, 40, 40);
        m_currentFg = m_defaultFg;
        m_currentBg = m_defaultBg;
        m_cursorColor = QColor(235, 219, 178);
        
        // Initialize cursor position
        m_cursorX = 0;
        m_cursorY = 0;
        m_cursorVisible = true;
        
        // Initialize escape sequence state
        m_escapeState = EscapeState::None;
        m_escapeSequence.clear();
        
        // Text attributes
        m_bold = false;
        m_italic = false;
        m_underline = false;
        
        // Start the PTY
        startPty();
        
        // Set up read timer
        m_readTimer = new QTimer(this);
        connect(m_readTimer, &QTimer::timeout, this, &TerminalWidget::readFromPty);
        m_readTimer->start(10); // Check every 10ms
        
        // Cursor blink timer
        m_cursorBlinkTimer = new QTimer(this);
        connect(m_cursorBlinkTimer, &QTimer::timeout, this, [this]() {
            m_cursorVisible = !m_cursorVisible;
            update(cursorRect());
        });
        m_cursorBlinkTimer->start(500); // Blink every 500ms
    }
    
    ~TerminalWidget() {
        if (m_childPid > 0) {
            kill(m_childPid, SIGTERM);
        }
        
        if (m_masterFd >= 0) {
            ::close(m_masterFd);  // Use global namespace for ::close to avoid QWidget::close
        }
        
        delete m_fontMetrics;
    }
    
protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter painter(this);
        painter.setFont(m_font);
        
        // Fill background
        painter.fillRect(event->rect(), m_defaultBg);
        
        // Draw text
        for (int y = 0; y < m_rows; y++) {
            for (int x = 0; x < m_cols; x++) {
                const TermChar &ch = m_buffer[y][x];
                
                // Draw background if different from default
                if (ch.background != m_defaultBg) {
                    painter.fillRect(x * m_charWidth, y * m_charHeight, 
                                    m_charWidth, m_charHeight, ch.background);
                }
                
                // Draw character if not a space
                if (!ch.character.isSpace()) {
                    QFont charFont = m_font;
                    
                    // Apply text styling
                    if (ch.bold) {
                        charFont.setBold(true);
                    }
                    if (ch.italic) {
                        charFont.setItalic(true);
                    }
                    
                    painter.setFont(charFont);
                    painter.setPen(ch.foreground);
                    painter.drawText(x * m_charWidth, y * m_charHeight + m_fontMetrics->ascent(), 
                                    ch.character);
                    
                    // Draw underline if needed
                    if (ch.underline) {
                        int underlineY = y * m_charHeight + m_fontMetrics->ascent() + 2;
                        painter.drawLine(x * m_charWidth, underlineY, 
                                        (x + 1) * m_charWidth, underlineY);
                    }
                }
            }
        }
        
        // Draw cursor
        if (m_cursorVisible && m_cursorY < m_buffer.size() && m_cursorX < m_buffer[m_cursorY].size()) {
            painter.fillRect(cursorRect(), m_cursorColor);
            
            // Draw character under cursor with inverted colors
            const TermChar &cursorChar = m_buffer[m_cursorY][m_cursorX];
            QFont cursorFont = m_font;
            
            if (cursorChar.bold) {
                cursorFont.setBold(true);
            }
            if (cursorChar.italic) {
                cursorFont.setItalic(true);
            }
            
            painter.setFont(cursorFont);
            painter.setPen(m_defaultBg);
            painter.drawText(m_cursorX * m_charWidth, 
                            m_cursorY * m_charHeight + m_fontMetrics->ascent(), 
                            cursorChar.character);
        }
    }
    
    void keyPressEvent(QKeyEvent *event) override {
        // Only process if we have a valid master file descriptor
        if (m_masterFd < 0) {
            return;
        }
        
        QByteArray data;
        
        // Handle special keys
        switch (event->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                data = "\r";
                break;
                
            case Qt::Key_Backspace:
                data = "\x7f"; // ASCII DEL
                break;
                
            case Qt::Key_Tab:
                data = "\t";
                break;
                
            case Qt::Key_Up:
                data = "\x1b[A"; // ANSI escape sequence for up arrow
                break;
                
            case Qt::Key_Down:
                data = "\x1b[B"; // ANSI escape sequence for down arrow
                break;
                
            case Qt::Key_Right:
                data = "\x1b[C"; // ANSI escape sequence for right arrow
                break;
                
            case Qt::Key_Left:
                data = "\x1b[D"; // ANSI escape sequence for left arrow
                break;
                
            case Qt::Key_Home:
                data = "\x1b[H"; // ANSI escape sequence for home
                break;
                
            case Qt::Key_End:
                data = "\x1b[F"; // ANSI escape sequence for end
                break;
                
            case Qt::Key_PageUp:
                data = "\x1b[5~"; // Page Up
                break;
                
            case Qt::Key_PageDown:
                data = "\x1b[6~"; // Page Down
                break;
                
            case Qt::Key_Delete:
                data = "\x1b[3~"; // Delete
                break;

            case Qt::Key_C:
                if (event->modifiers() & Qt::ControlModifier) {
                    // Ctrl+C - send SIGINT
                    data = QByteArray(1, 3); // ASCII ETX (End of Text)
                    m_pendingNewline = false; // Clear any pending newline to avoid issues
                } else {
                    data = event->text().toUtf8();
                }
                break;
                
            default:
                if (event->modifiers() & Qt::ControlModifier) {
                    if (event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
                        // Control key combinations (Ctrl+A through Ctrl+Z)
                        char ch = event->key() - Qt::Key_A + 1;
                        data = QByteArray(1, ch);
                    } else if (event->key() == Qt::Key_BracketLeft) {
                        // Ctrl+[
                        data = QByteArray(1, '\x1b');
                    }
                } else {
                    // Normal text input
                    data = event->text().toUtf8();
                }
        }
        
        // Send data to the PTY
        if (!data.isEmpty()) {
            write(m_masterFd, data.constData(), data.size());
        }
    }
    
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        
        // Calculate the new terminal dimensions
        int newCols = event->size().width() / m_charWidth;
        int newRows = event->size().height() / m_charHeight;
        
        // Make sure we have at least 1x1 dimensions
        newCols = qMax(1, newCols);
        newRows = qMax(1, newRows);
        
        // Check if the dimensions actually changed
        if (newCols != m_cols || newRows != m_rows) {
            // Resize the terminal buffer
            resizeBuffer(newRows, newCols);
            
            // Notify the PTY of the new size
            if (m_masterFd >= 0) {
                struct winsize ws;
                ws.ws_col = newCols;
                ws.ws_row = newRows;
                ws.ws_xpixel = event->size().width();
                ws.ws_ypixel = event->size().height();
                ioctl(m_masterFd, TIOCSWINSZ, &ws);
            }
        }
    }
    
private:
    // ANSI escape sequence state
    enum class EscapeState {
        None,           // Not in an escape sequence
        Escape,         // Just received ESC
        Bracket,        // Received ESC [
        Parameter,      // Collecting parameters
        OSC,            // Operating System Command (ESC ])
        OSCParameter    // Collecting OSC parameters
    };
    
    QRect cursorRect() const {
        return QRect(m_cursorX * m_charWidth, m_cursorY * m_charHeight, m_charWidth, m_charHeight);
    }
    
    void initializeColorPalette() {
        // Basic 16 colors
        m_colorPalette.resize(256);
        
        // Standard colors (0-7)
        m_colorPalette[0] = QColor(40, 40, 40);      // Black
        m_colorPalette[1] = QColor(204, 36, 29);     // Red
        m_colorPalette[2] = QColor(152, 151, 26);    // Green
        m_colorPalette[3] = QColor(215, 153, 33);    // Yellow
        m_colorPalette[4] = QColor(69, 133, 136);    // Blue
        m_colorPalette[5] = QColor(177, 98, 134);    // Magenta
        m_colorPalette[6] = QColor(104, 157, 106);   // Cyan
        m_colorPalette[7] = QColor(168, 153, 132);   // White
        
        // Bright colors (8-15)
        m_colorPalette[8] = QColor(146, 131, 116);   // Bright Black (Gray)
        m_colorPalette[9] = QColor(251, 73, 52);     // Bright Red
        m_colorPalette[10] = QColor(184, 187, 38);   // Bright Green
        m_colorPalette[11] = QColor(250, 189, 47);   // Bright Yellow
        m_colorPalette[12] = QColor(131, 165, 152);  // Bright Blue
        m_colorPalette[13] = QColor(211, 134, 155);  // Bright Magenta
        m_colorPalette[14] = QColor(142, 192, 124);  // Bright Cyan
        m_colorPalette[15] = QColor(235, 219, 178);  // Bright White
        
        // Generate 216 colors for 6x6x6 color cube (16-231)
        int index = 16;
        for (int r = 0; r < 6; r++) {
            for (int g = 0; g < 6; g++) {
                for (int b = 0; b < 6; b++) {
                    int red = r == 0 ? 0 : (r * 40 + 55);
                    int green = g == 0 ? 0 : (g * 40 + 55);
                    int blue = b == 0 ? 0 : (b * 40 + 55);
                    m_colorPalette[index++] = QColor(red, green, blue);
                }
            }
        }
        
        // Generate 24 grayscale colors (232-255)
        for (int i = 0; i < 24; i++) {
            int value = i * 10 + 8;
            m_colorPalette[232 + i] = QColor(value, value, value);
        }
    }
    
    void startPty() {
        // Create a pseudo-terminal
        m_childPid = forkpty(&m_masterFd, nullptr, nullptr, nullptr);
        
        if (m_childPid == -1) {
            // Fork failed
            qDebug() << "Failed to fork PTY: " << strerror(errno);
            return;
        } else if (m_childPid == 0) {
            // Child process - execute the shell
            char *shell = getenv("SHELL");
            if (!shell) shell = const_cast<char*>("/bin/bash");
            
            // Set TERM environment variable
            setenv("TERM", "xterm-256color", 1);
            
            // Set ZSH shell prompt to not display % at the end
            if (strstr(shell, "zsh")) {
                setenv("PROMPT", "%~ $ ", 1);
            }
            
            // Execute the shell
            execl(shell, shell, "-l", nullptr);
            
            // If execl returns, there was an error
            perror("execl");
            exit(1);
        }
        
        // Parent process continues here
        // Set the PTY to non-blocking mode
        int flags = fcntl(m_masterFd, F_GETFL, 0);
        fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);
    }
    
    void resizeBuffer(int newRows, int newCols) {
        // Save old buffer dimensions
        int oldRows = m_rows;
        int oldCols = m_cols;
        
        // Create a new buffer with the new dimensions
        QVector<QVector<TermChar>> newBuffer(newRows);
        for (int i = 0; i < newRows; i++) {
            newBuffer[i].resize(newCols);
        }
        
        // Copy the data from the old buffer to the new one
        for (int y = 0; y < qMin(oldRows, newRows); y++) {
            for (int x = 0; x < qMin(oldCols, newCols); x++) {
                newBuffer[y][x] = m_buffer[y][x];
            }
        }
        
        // Update the dimensions and buffer
        m_rows = newRows;
        m_cols = newCols;
        m_buffer = newBuffer;
        
        // Make sure the cursor is still in bounds
        m_cursorX = qMin(m_cursorX, m_cols - 1);
        m_cursorY = qMin(m_cursorY, m_rows - 1);
    }
    
    void processOutput(const QByteArray &data) {
        // We need to handle UTF-8 sequences properly
        int i = 0;
        while (i < data.size()) {
            char c = data[i++];
            
            // Check if we're in the middle of a UTF-8 sequence
            if (m_utf8Remaining > 0) {
                if ((c & 0xC0) == 0x80) {
                    // This is a UTF-8 continuation byte
                    m_utf8Buffer += c;
                    m_utf8Remaining--;
                    
                    if (m_utf8Remaining == 0) {
                        // Process the complete UTF-8 sequence
                        processUtf8Sequence(m_utf8Buffer);
                        m_utf8Buffer.clear();
                    }
                    continue;
                } else {
                    // Invalid UTF-8 sequence, reset and process as normal
                    m_utf8Remaining = 0;
                    m_utf8Buffer.clear();
                }
            }
            
            // Start of UTF-8 sequence
            if ((c & 0x80) != 0) {
                if ((c & 0xE0) == 0xC0) {
                    // 2-byte sequence
                    m_utf8Remaining = 1;
                    m_utf8Buffer = QByteArray(1, c);
                } else if ((c & 0xF0) == 0xE0) {
                    // 3-byte sequence
                    m_utf8Remaining = 2;
                    m_utf8Buffer = QByteArray(1, c);
                } else if ((c & 0xF8) == 0xF0) {
                    // 4-byte sequence
                    m_utf8Remaining = 3;
                    m_utf8Buffer = QByteArray(1, c);
                } else {
                    // Invalid UTF-8 byte, treat as ASCII
                    processChar(c);
                }
            } else {
                // ASCII character
                processChar(c);
            }
        }
        
        // Redraw the terminal
        update();
    }
    
    void processUtf8Sequence(const QByteArray &utf8Bytes) {
        // Convert UTF-8 bytes to Unicode code point
        QString str = QString::fromUtf8(utf8Bytes);
        if (str.isEmpty()) {
            return;
        }
        
        QChar ch = str[0];
        
        // Store character with attributes
        m_buffer[m_cursorY][m_cursorX].character = ch;
        m_buffer[m_cursorY][m_cursorX].foreground = m_currentFg;
        m_buffer[m_cursorY][m_cursorX].background = m_currentBg;
        m_buffer[m_cursorY][m_cursorX].bold = m_bold;
        m_buffer[m_cursorY][m_cursorX].italic = m_italic;
        m_buffer[m_cursorY][m_cursorX].underline = m_underline;
        
        // Move cursor forward
        m_cursorX++;
        
        // Check if it's a wide character (like CJK)
        // A simple heuristic for CJK characters - they generally have high Unicode values
        // This is not perfect but works for most common cases
        uint unicode = ch.unicode();
        if ((unicode >= 0x3000 && unicode <= 0x9FFF) || // CJK unified ideographs, symbols, etc.
            (unicode >= 0xAC00 && unicode <= 0xD7AF) || // Hangul
            (unicode >= 0xF900 && unicode <= 0xFAFF) || // CJK compatibility ideographs
            (unicode >= 0xFF00 && unicode <= 0xFFEF) || // Halfwidth and fullwidth forms
            (unicode >= 0x20000 && unicode <= 0x2FFFF)) { // CJK extension areas
            
            // Mark the next cell as part of this character
            if (m_cursorX < m_cols) {
                m_buffer[m_cursorY][m_cursorX].character = QChar::Space;
                m_cursorX++;
            }
        }
        
        // Handle line wrapping
        if (m_cursorX >= m_cols) {
            m_cursorX = 0;
            m_cursorY++;
            if (m_cursorY >= m_rows) {
                scrollUp();
                m_cursorY = m_rows - 1;
            }
        }
    }
    
    void processChar(char c) {
        // Process character based on current escape state
        switch (m_escapeState) {
            case EscapeState::None:
                if (c == '\x1b') { // ESC character
                    m_escapeState = EscapeState::Escape;
                    m_escapeSequence.clear();
                } else {
                    processRegularChar(c);
                }
                break;
                
            case EscapeState::Escape:
                if (c == '[') {
                    m_escapeState = EscapeState::Bracket;
                } else if (c == ']') {
                    // OSC sequence (Operating System Command)
                    m_escapeState = EscapeState::OSC;
                    m_escapeSequence.clear();
                } else if (c == '%') {
                    // Ignore % character in escape sequences - common in some terminals
                    // Just stay in escape state for now
                } else {
                    // Simple ESC sequence (not fully supported)
                    m_escapeState = EscapeState::None;
                    processRegularChar(c);
                }
                break;
                
            case EscapeState::Bracket:
                if ((c >= '0' && c <= '9') || c == ';' || c == '?' || c == ' ' || c == '>') {
                    // Collect parameter
                    m_escapeSequence += c;
                    m_escapeState = EscapeState::Parameter;
                } else {
                    // Single character command
                    processEscapeSequence(c, m_escapeSequence);
                    m_escapeState = EscapeState::None;
                }
                break;
                
            case EscapeState::Parameter:
                if ((c >= '0' && c <= '9') || c == ';' || c == '?' || c == ' ' || c == '>' || c == '=') {
                    // Continue collecting parameter
                    m_escapeSequence += c;
                } else {
                    // Process the complete escape sequence
                    processEscapeSequence(c, m_escapeSequence);
                    m_escapeState = EscapeState::None;
                }
                break;
                
            case EscapeState::OSC:
                if (c >= '0' && c <= '9') {
                    m_escapeSequence += c;
                } else if (c == ';') {
                    m_escapeSequence += c;
                    m_escapeState = EscapeState::OSCParameter;
                } else {
                    // Invalid OSC sequence
                    m_escapeState = EscapeState::None;
                }
                break;
                
            case EscapeState::OSCParameter:
                if (c == '\x07' || (c == '\\' && m_escapeSequence.endsWith("\x1b"))) {
                    // BEL or ESC \ terminates OSC sequence
                    // Process OSC command - currently ignored
                    m_escapeState = EscapeState::None;
                } else {
                    m_escapeSequence += c;
                }
                break;
        }
    }
    
    void processRegularChar(char c) {
        // Handle basic terminal output
        switch (c) {
            case '\r': // Carriage return
                m_cursorX = 0;
                // Handle CR+LF as a single operation to prevent dangling %
                if (m_pendingNewline) {
                    m_pendingNewline = false;
                }
                break;
                
            case '\n': // Line feed
                if (m_pendingNewline) {
                    // Already had a newline pending, just process it
                    m_pendingNewline = false;
                } else {
                    // Mark that we've seen a newline
                    m_pendingNewline = true;
                    
                    // Process immediately
                    m_cursorY++;
                    if (m_cursorY >= m_rows) {
                        // Need to scroll the buffer
                        scrollUp();
                        m_cursorY = m_rows - 1;
                    }
                    // Important: Reset cursor X position for proper prompt alignment
                    m_cursorX = 0;
                }
                break;
                
            case '\b': // Backspace
                if (m_cursorX > 0) {
                    m_cursorX--;
                }
                break;
                
            case '\t': // Tab
                // Set tab stops every 8 characters - important for programs like fastfetch
                m_cursorX = ((m_cursorX + 8) / 8) * 8;
                if (m_cursorX >= m_cols) {
                    m_cursorX = 0;
                    m_cursorY++;
                    if (m_cursorY >= m_rows) {
                        scrollUp();
                        m_cursorY = m_rows - 1;
                    }
                }
                break;
                
            case '\a': // Bell
                // Would normally beep
                break;
                
            default:
                if (c >= 32) { // Printable character
                    // Clear any pending newline state
                    m_pendingNewline = false;
                    
                    // Store the character in the buffer with current attributes
                    m_buffer[m_cursorY][m_cursorX].character = QChar(c);
                    m_buffer[m_cursorY][m_cursorX].foreground = m_currentFg;
                    m_buffer[m_cursorY][m_cursorX].background = m_currentBg;
                    m_buffer[m_cursorY][m_cursorX].bold = m_bold;
                    m_buffer[m_cursorY][m_cursorX].italic = m_italic;
                    m_buffer[m_cursorY][m_cursorX].underline = m_underline;
                    
                    // Move cursor forward
                    m_cursorX++;
                    
                    if (m_cursorX >= m_cols) {
                        m_cursorX = 0;
                        m_cursorY++;
                        if (m_cursorY >= m_rows) {
                            scrollUp();
                            m_cursorY = m_rows - 1;
                        }
                    }
                }
        }
    }
    
    void processEscapeSequence(char finalChar, const QString &parameters) {
        // Parse the parameter string into integers
        QStringList paramList = parameters.split(';');
        QVector<int> params;
        
        // Check for private mode sequences
        bool privateMode = parameters.startsWith('?') || parameters.startsWith('>');
        QString cleanParams = parameters;
        if (privateMode) {
            cleanParams = parameters.mid(1);
            paramList = cleanParams.split(';');
        }
        
        for (const QString &param : paramList) {
            bool ok;
            int value = param.toInt(&ok);
            params.append(ok ? value : 0);
        }
        
        // Default parameter is 0 if none specified
        if (params.isEmpty()) {
            params.append(0);
        }
        
        // Process based on the final character
        switch (finalChar) {
            case 'm': // SGR - Select Graphic Rendition
                processSGR(params);
                break;
                
            case 'H': // CUP - Cursor Position
            case 'f': // HVP - Horizontal and Vertical Position
                if (params.size() >= 2) {
                    // Parameters are 1-based, convert to 0-based
                    m_cursorY = qBound(0, params[0] - 1, m_rows - 1);
                    m_cursorX = qBound(0, params[1] - 1, m_cols - 1);
                } else {
                    // Move to home position (0,0)
                    m_cursorX = 0;
                    m_cursorY = 0;
                }
                break;
                
            case 'A': // CUU - Cursor Up
                m_cursorY = qMax(0, m_cursorY - qMax(1, params[0]));
                break;
                
            case 'B': // CUD - Cursor Down
                m_cursorY = qMin(m_rows - 1, m_cursorY + qMax(1, params[0]));
                break;
                
            case 'C': // CUF - Cursor Forward
                m_cursorX = qMin(m_cols - 1, m_cursorX + qMax(1, params[0]));
                break;
                
            case 'D': // CUB - Cursor Back
                m_cursorX = qMax(0, m_cursorX - qMax(1, params[0]));
                break;
                
            case 'G': // CHA - Cursor Horizontal Absolute
                m_cursorX = qBound(0, params[0] - 1, m_cols - 1);
                break;
                
            case 'J': // ED - Erase in Display
                switch (params[0]) {
                    case 0: // Clear from cursor to end of screen
                        clearScreen(m_cursorY, m_cursorX, m_rows - 1, m_cols - 1);
                        break;
                    case 1: // Clear from beginning of screen to cursor
                        clearScreen(0, 0, m_cursorY, m_cursorX);
                        break;
                    case 2: // Clear entire screen
                    case 3: // Clear entire screen and scrollback (treated the same)
                        clearScreen(0, 0, m_rows - 1, m_cols - 1);
                        break;
                }
                break;
                
            case 'K': // EL - Erase in Line
                switch (params[0]) {
                    case 0: // Clear from cursor to end of line
                        clearLine(m_cursorY, m_cursorX, m_cols - 1);
                        break;
                    case 1: // Clear from beginning of line to cursor
                        clearLine(m_cursorY, 0, m_cursorX);
                        break;
                    case 2: // Clear entire line
                        clearLine(m_cursorY, 0, m_cols - 1);
                        break;
                }
                break;
                
            case 's': // SCP - Save Cursor Position
                m_savedCursorX = m_cursorX;
                m_savedCursorY = m_cursorY;
                break;
                
            case 'u': // RCP - Restore Cursor Position
                m_cursorX = m_savedCursorX;
                m_cursorY = m_savedCursorY;
                break;
                
            case 'l': // Reset Mode
            case 'h': // Set Mode
                if (privateMode) {
                    // Handle private mode sequences
                    for (int param : params) {
                        switch (param) {
                            case 25: // Show/hide cursor
                                m_cursorVisible = (finalChar == 'h');
                                break;
                            // Add more private mode handlers as needed
                        }
                    }
                }
                break;
                
            case 'd': // VPA - Line Position Absolute
                m_cursorY = qBound(0, params[0] - 1, m_rows - 1);
                break;
                
            case 'r': // DECSTBM - Set Top and Bottom Margins (scrolling region)
                // Ignoring for now, but important for some complex terminal programs
                break;
                
            case 'n': // DSR - Device Status Report
                if (params[0] == 6) {
                    // Report cursor position - not implementing response
                }
                break;
        }
    }
    
    void processSGR(const QVector<int> &params) {
        // Process SGR parameters
        for (int i = 0; i < params.size(); i++) {
            int param = params[i];
            
            switch (param) {
                case 0: // Reset all attributes
                    m_currentFg = m_defaultFg;
                    m_currentBg = m_defaultBg;
                    m_bold = false;
                    m_italic = false;
                    m_underline = false;
                    break;
                    
                case 1: // Bold
                    m_bold = true;
                    break;
                    
                case 3: // Italic
                    m_italic = true;
                    break;
                    
                case 4: // Underline
                    m_underline = true;
                    break;
                    
                case 22: // Normal intensity (not bold)
                    m_bold = false;
                    break;
                    
                case 23: // Not italic
                    m_italic = false;
                    break;
                    
                case 24: // Not underlined
                    m_underline = false;
                    break;
                    
                case 30: case 31: case 32: case 33: // Foreground colors
                case 34: case 35: case 36: case 37:
                    m_currentFg = m_colorPalette[param - 30];
                    break;
                    
                case 38: // Extended foreground color
                    if (i + 2 < params.size() && params[i + 1] == 5) { // 256 color mode
                        int colorIndex = params[i + 2];
                        if (colorIndex >= 0 && colorIndex < 256) {
                            m_currentFg = m_colorPalette[colorIndex];
                        }
                        i += 2; // Skip the next two parameters
                    } else if (i + 4 < params.size() && params[i + 1] == 2) { // RGB mode
                        int r = params[i + 2];
                        int g = params[i + 3];
                        int b = params[i + 4];
                        m_currentFg = QColor(r, g, b);
                        i += 4; // Skip the next four parameters
                    }
                    break;
                    
                case 39: // Default foreground color
                    m_currentFg = m_defaultFg;
                    break;
                    
                case 40: case 41: case 42: case 43: // Background colors
                case 44: case 45: case 46: case 47:
                    m_currentBg = m_colorPalette[param - 40];
                    break;
                    
                case 48: // Extended background color
                    if (i + 2 < params.size() && params[i + 1] == 5) { // 256 color mode
                        int colorIndex = params[i + 2];
                        if (colorIndex >= 0 && colorIndex < 256) {
                            m_currentBg = m_colorPalette[colorIndex];
                        }
                        i += 2; // Skip the next two parameters
                    } else if (i + 4 < params.size() && params[i + 1] == 2) { // RGB mode
                        int r = params[i + 2];
                        int g = params[i + 3];
                        int b = params[i + 4];
                        m_currentBg = QColor(r, g, b);
                        i += 4; // Skip the next four parameters
                    }
                    break;
                    
                case 49: // Default background color
                    m_currentBg = m_defaultBg;
                    break;
                    
                case 90: case 91: case 92: case 93: // Bright foreground colors
                case 94: case 95: case 96: case 97:
                    m_currentFg = m_colorPalette[param - 90 + 8];
                    break;
                    
                case 100: case 101: case 102: case 103: // Bright background colors
                case 104: case 105: case 106: case 107:
                    m_currentBg = m_colorPalette[param - 100 + 8];
                    break;
            }
        }
    }
    
    void clearScreen(int startRow, int startCol, int endRow, int endCol) {
        for (int y = startRow; y <= endRow; y++) {
            for (int x = (y == startRow ? startCol : 0); 
                 x <= (y == endRow ? endCol : m_cols - 1); x++) {
                m_buffer[y][x] = TermChar();
            }
        }
    }
    
    void clearLine(int row, int startCol, int endCol) {
        for (int x = startCol; x <= endCol; x++) {
            m_buffer[row][x] = TermChar();
        }
    }
    
    void scrollUp() {
        // Move all lines up one position
        for (int y = 0; y < m_rows - 1; y++) {
            m_buffer[y] = m_buffer[y + 1];
        }
        
        // Clear the bottom line
        for (int x = 0; x < m_cols; x++) {
            m_buffer[m_rows - 1][x] = TermChar();
        }
    }
    
private slots:
    void readFromPty() {
        if (m_masterFd < 0) {
            return;
        }
        
        // Read data from the PTY
        char buffer[4096];
        ssize_t bytesRead = read(m_masterFd, buffer, sizeof(buffer) - 1);
        
        if (bytesRead > 0) {
            // Null-terminate the data
            buffer[bytesRead] = '\0';
            
            // Process the data
            processOutput(QByteArray(buffer, bytesRead));
        } else if (bytesRead == -1 && errno != EAGAIN) {
            // An error occurred
            qDebug() << "Error reading from PTY: " << strerror(errno);
        }
    }
    
private:
    QFont m_font;
    QFontMetrics *m_fontMetrics;
    int m_charWidth;
    int m_charHeight;
    
    int m_rows;
    int m_cols;
    QVector<QVector<TermChar>> m_buffer;
    
    QVector<QColor> m_colorPalette;
    QColor m_defaultFg;
    QColor m_defaultBg;
    QColor m_currentFg;
    QColor m_currentBg;
    QColor m_cursorColor;
    
    int m_cursorX;
    int m_cursorY;
    int m_savedCursorX = 0;
    int m_savedCursorY = 0;
    bool m_cursorVisible;
    bool m_pendingNewline = false; // Track newline state
    
    // Text attributes
    bool m_bold;
    bool m_italic;
    bool m_underline;
    
    // UTF-8 processing
    int m_utf8Remaining = 0;
    uint32_t m_utf8CodePoint = 0;
    QByteArray m_utf8Buffer;
    
    // Escape sequence processing
    EscapeState m_escapeState;
    QString m_escapeSequence;
    
    pid_t m_childPid;
    int m_masterFd;
    
    QTimer *m_readTimer;
    QTimer *m_cursorBlinkTimer;
};

// Include moc file since we're using Q_OBJECT
#include "main.moc"

// Replace BUILD_COMMAND() with regular main
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    QMainWindow window;
    window.setWindowTitle("KorzeTerm");
    window.resize(800, 600);
    
    QWidget *centralWidget = new QWidget(&window);
    window.setCentralWidget(centralWidget);
    
    QVBoxLayout *layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    TerminalWidget *terminal = new TerminalWidget(centralWidget);
    layout->addWidget(terminal);
    
    window.show();
    return app.exec();
} 