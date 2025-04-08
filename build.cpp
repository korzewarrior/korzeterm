#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

int main() {
    printf("Building KorzeTerm...\n");
    
    // Generate the moc file (Qt Meta-Object Compiler)
    printf("Generating Meta-Object file...\n");
    int moc_result = system("moc main.cpp -o main.moc");
    
    if (moc_result != 0) {
        printf("Failed to generate Meta-Object file. Make sure Qt5 development tools are installed.\n");
        printf("Try running: sudo pacman -S qt5-base qt5-tools\n");
        return 1;
    }
    
    // Build the terminal emulator
    const char* cmd = "g++ -std=c++11 -o korzeterm main.cpp $(pkg-config --cflags --libs Qt5Widgets Qt5Core Qt5Gui) -Wall -Wextra";
    printf("Running: %s\n", cmd);
    
    int result = system(cmd);
    
    if (result == 0) {
        printf("Build successful!\n");
        printf("Running KorzeTerm...\n");
        system("./korzeterm");
    } else {
        printf("Build failed!\n");
        printf("Make sure you have installed all required packages:\n");
        printf("  - base-devel (for g++ and build tools)\n");
        printf("  - qt5-base (for Qt libraries)\n");
        printf("  - qt5-tools (for moc compiler)\n");
    }
    
    return result;
} 