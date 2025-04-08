// Explicitly include required Qt headers
#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QProcess>
#include <QFontDatabase>
#include <QTimer>
#include <QTextEdit>
#include <QScrollBar>
#include <QKeyEvent>  // Ensure QKeyEvent is fully included

// Terminal emulator implementation
class TerminalWidget : public QTextEdit {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr) : QTextEdit(parent) {
        // Setup appearance
        setStyleSheet("background-color: #282828; color: #EBDBB2;");
        
        // Use monospace font
        QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPointSize(10);
        setFont(font);
        
        // Setup document
        document()->setMaximumBlockCount(5000); // Limit scrollback
        
        // Make read-only but still focusable
        setReadOnly(false); // Allow input directly
        
        // Create and setup process
        process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);
        
        // Connect signals and slots
        connect(process, &QProcess::readyReadStandardOutput, this, &TerminalWidget::readOutput);
        // Use old-style connection for overloaded signals
        connect(process, SIGNAL(finished(int, QProcess::ExitStatus)), 
                this, SLOT(handleFinished(int, QProcess::ExitStatus)));
        
        // Start shell
        QString shell = qgetenv("SHELL");
        if (shell.isEmpty()) shell = "/bin/bash";
        
        QStringList env = QProcess::systemEnvironment();
        env << "TERM=vt100";
        process->setEnvironment(env);
        
        QStringList args;
        args << "-l"; // Login shell
        process->start(shell, args);
        
        // Display startup message
        insertPlainText("KorzeTerm 0.1 - Starting shell...\n");
        setFocus();
    }
    
    ~TerminalWidget() {
        if (process->state() == QProcess::Running) {
            process->terminate();
            process->waitForFinished(500);
            if (process->state() == QProcess::Running) {
                process->kill();
            }
        }
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (process->state() != QProcess::Running) {
            QTextEdit::keyPressEvent(event);
            return;
        }
        
        // Handle special keys
        switch (event->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                process->write("\r");
                break;
                
            case Qt::Key_Backspace:
                process->write("\x7f"); // ASCII DEL
                break;
                
            case Qt::Key_Tab:
                process->write("\t");
                break;
                
            case Qt::Key_Up:
                process->write("\x1b[A"); // ANSI escape sequence for up arrow
                break;
                
            case Qt::Key_Down:
                process->write("\x1b[B"); // ANSI escape sequence for down arrow
                break;
                
            case Qt::Key_Right:
                process->write("\x1b[C"); // ANSI escape sequence for right arrow
                break;
                
            case Qt::Key_Left:
                process->write("\x1b[D"); // ANSI escape sequence for left arrow
                break;
                
            case Qt::Key_Home:
                process->write("\x1b[H"); // ANSI escape sequence for home
                break;
                
            case Qt::Key_End:
                process->write("\x1b[F"); // ANSI escape sequence for end
                break;
                
            case Qt::Key_PageUp:
                process->write("\x1b[5~"); // Page Up
                break;
                
            case Qt::Key_PageDown:
                process->write("\x1b[6~"); // Page Down
                break;
                
            case Qt::Key_Delete:
                process->write("\x1b[3~"); // Delete
                break;
                
            default:
                if (event->modifiers() & Qt::ControlModifier) {
                    if (event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
                        // Control key combinations (Ctrl+A through Ctrl+Z)
                        char ch = event->key() - Qt::Key_A + 1;
                        process->write(QByteArray(1, ch));
                    } else if (event->key() == Qt::Key_BracketLeft) {
                        // Ctrl+[
                        process->write(QByteArray(1, '\x1b'));
                    }
                } else {
                    // Normal text input
                    process->write(event->text().toUtf8());
                }
        }
    }
    
    void resizeEvent(QResizeEvent *event) override {
        QTextEdit::resizeEvent(event);
        
        // Update terminal size
        QFontMetrics fm(font());
        
        // Calculate terminal dimensions
        int cols = width() / fm.horizontalAdvance('M');
        int rows = height() / fm.height();
        
        // Use the calculated dimensions (commented out for now)
        Q_UNUSED(cols);
        Q_UNUSED(rows);
        
        // Use ioctl to set terminal size if needed
        if (process->state() == QProcess::Running) {
            // We'll avoid using ioctl directly and set environment variables instead
            // This is just a simplified approach
        }
    }

private slots:
    void readOutput() {
        QByteArray data = process->readAllStandardOutput();
        if (data.isEmpty()) return;
        
        // Auto-scroll if at bottom
        QScrollBar *scrollbar = verticalScrollBar();
        bool atBottom = scrollbar->value() == scrollbar->maximum();
        
        // Move cursor to end and insert the text
        moveCursor(QTextCursor::End);
        
        // Convert invalid UTF-8 sequences to placeholders
        QString text = QString::fromUtf8(data);
        
        // Just append the text - keeping it simple
        insertPlainText(text);
        
        // Auto-scroll if we were at the bottom
        if (atBottom) {
            scrollbar->setValue(scrollbar->maximum());
        }
    }
    
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus) {
        Q_UNUSED(exitCode);
        Q_UNUSED(exitStatus);
        
        // Just indicate process has ended
        setReadOnly(true);
        append("\nProcess finished.");
    }

private:
    QProcess *process;
};

// Include moc file since we're using Q_OBJECT
#include "main.moc"

// Replace BUILD_COMMAND() with regular main
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    QMainWindow window;
    window.setWindowTitle("KorzeTerm");
    window.resize(800, 500);
    
    QWidget *centralWidget = new QWidget(&window);
    window.setCentralWidget(centralWidget);
    
    QVBoxLayout *layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(2, 2, 2, 2);
    
    TerminalWidget *terminal = new TerminalWidget(centralWidget);
    layout->addWidget(terminal);
    
    window.show();
    return app.exec();
} 