const int MAX_PATH = 50;

typedef struct {
    int state;
    int prev_length;
    int length;
    int hi_length;
    char path[50];
} PathInfo;