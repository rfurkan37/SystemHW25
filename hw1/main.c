#include "fileManager.h"
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        displayHelp();
        return 0;
    }

    if (strcmp(argv[1], "createDir") == 0 && argc == 3) {
        createDirectory(argv[2]);
    } 
    else if (strcmp(argv[1], "createFile") == 0 && argc == 3) {
        createFile(argv[2]);
    } 
    else if (strcmp(argv[1], "listDir") == 0 && argc == 3) {
        listDirectory(argv[2]);
    } 
    else if (strcmp(argv[1], "listFilesByExtension") == 0 && argc == 4) {
        listFilesByExtension(argv[2], argv[3]);
    } 
    else if (strcmp(argv[1], "readFile") == 0 && argc == 3) {
        readFile(argv[2]);
    } 
    else if (strcmp(argv[1], "appendToFile") == 0 && argc == 4) {
        appendToFile(argv[2], argv[3]);
    } 
    else if (strcmp(argv[1], "deleteFile") == 0 && argc == 3) {
        deleteFile(argv[2]);
    } 
    else if (strcmp(argv[1], "deleteDir") == 0 && argc == 3) {
        deleteDirectory(argv[2]);
    } 
    else if (strcmp(argv[1], "showLogs") == 0 && argc == 2) {
        showLogs();
    } 
    else {
        displayHelp();
    }

    return 0;
}
