/* file_walker_fixed.c - Safe recursive file search */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

/* Safe buffer sizes */
#define MAX_PATH 4096
#define MAX_LINE 2048
#define MAX_KEYWORDS 20
#define MAX_MATCHES_PER_FILE 50

/* Search options */
typedef struct {
    char keywords[MAX_KEYWORDS][256];
    int keyword_count;
    int case_sensitive;
    int recursive;
    int search_filenames;
    int search_content;
    int show_line_numbers;
    int max_depth;
    int only_matching_files;
    int count_only;
    long min_size;
    long max_size;
    char file_pattern[256];
    char start_dir[MAX_PATH];
} SearchOptions;

/* File match result */
typedef struct {
    int line_number;
    char line_content[MAX_LINE];
} FileMatch;

/* Global statistics */
typedef struct {
    long files_searched;
    long files_matched;
    long total_matches;
    long total_size;
    time_t start_time;
} SearchStats;

/* Function prototypes */
void init_options(SearchOptions *opts);
void parse_arguments(int argc, char *argv[], SearchOptions *opts);
void search_directory(const char *path, int depth, 
                     const SearchOptions *opts, SearchStats *stats);
int search_file(const char *filename, const SearchOptions *opts, 
               SearchStats *stats);
int matches_pattern(const char *filename, const char *pattern);
void print_help(void);
void print_stats(const SearchStats *stats);

/* Safe case-insensitive comparison */
int strcasecmp_safe(const char *s1, const char *s2) {
    if (!s1 || !s2) return -1;
    
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/* Safe case-insensitive string search */
const char *strstr_case(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return NULL;
    
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        
        while (*h && *n && 
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        
        if (!*n) return haystack;
    }
    
    return NULL;
}

/* Initialize default options */
void init_options(SearchOptions *opts) {
    memset(opts, 0, sizeof(SearchOptions));
    opts->keyword_count = 0;
    opts->case_sensitive = 1;
    opts->recursive = 1;
    opts->search_filenames = 1;
    opts->search_content = 1;
    opts->show_line_numbers = 1;
    opts->max_depth = -1;
    opts->only_matching_files = 0;
    opts->count_only = 0;
    opts->min_size = 0;
    opts->max_size = -1;
    opts->file_pattern[0] = '\0';
    strcpy(opts->start_dir, ".");
}

/* Parse command line arguments safely */
void parse_arguments(int argc, char *argv[], SearchOptions *opts) {
    int i = 1;
    
    while (i < argc) {
        if (argv[i][0] == '-') {
            switch (argv[i][1]) {
                case 'i': 
                    opts->case_sensitive = 0; 
                    break;
                case 'r': 
                    opts->recursive = 1; 
                    break;
                case 'l': 
                    opts->only_matching_files = 1; 
                    break;
                case 'c': 
                    opts->count_only = 1; 
                    break;
                case 'n': 
                    opts->show_line_numbers = 1; 
                    break;
                case 'f': 
                    if (i + 1 < argc) {
                        strncpy(opts->file_pattern, argv[++i], 
                                sizeof(opts->file_pattern) - 1);
                    }
                    break;
                case 'd':
                    if (i + 1 < argc) {
                        opts->max_depth = atoi(argv[++i]);
                    }
                    break;
                case 's':
                    if (i + 1 < argc) {
                        opts->min_size = atol(argv[++i]);
                    }
                    break;
                case 'S':
                    if (i + 1 < argc) {
                        opts->max_size = atol(argv[++i]);
                    }
                    break;
                case 'h':
                    print_help();
                    exit(EXIT_SUCCESS);
                default:
                    fprintf(stderr, "Unknown option: %s\n", argv[i]);
                    print_help();
                    exit(EXIT_FAILURE);
            }
            i++;
        } else {
            /* First non-option argument could be directory */
            if (opts->keyword_count == 0 && access(argv[i], F_OK) == 0) {
                struct stat st;
                if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) {
                    strncpy(opts->start_dir, argv[i], sizeof(opts->start_dir) - 1);
                    i++;
                    continue;
                }
            }
            
            /* Otherwise, treat as keyword */
            if (opts->keyword_count < MAX_KEYWORDS) {
                strncpy(opts->keywords[opts->keyword_count], argv[i], 255);
                opts->keywords[opts->keyword_count][255] = '\0';
                opts->keyword_count++;
            }
            i++;
        }
    }
    
    if (opts->keyword_count == 0) {
        fprintf(stderr, "Error: No keywords specified\n");
        print_help();
        exit(EXIT_FAILURE);
    }
}

/* Check if filename matches pattern */
int matches_pattern(const char *filename, const char *pattern) {
    if (!filename || !pattern) return 0;
    if (pattern[0] == '\0') return 1;
    
    const char *basename = strrchr(filename, '/');
    basename = basename ? basename + 1 : filename;
    
    /* Simple wildcard matching */
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *ext = strrchr(basename, '.');
        if (!ext) return 0;
        return strcasecmp_safe(ext + 1, pattern + 2) == 0;
    }
    
    return strcasecmp_safe(basename, pattern) == 0;
}

/* Search a single file safely */
int search_file(const char *filename, const SearchOptions *opts, 
               SearchStats *stats) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        return 0;
    }
    
    /* Check file size constraints */
    struct stat st;
    if (fstat(fileno(file), &st) == 0) {
        stats->total_size += st.st_size;
        
        if ((opts->min_size > 0 && st.st_size < opts->min_size) ||
            (opts->max_size >= 0 && st.st_size > opts->max_size)) {
            fclose(file);
            return 0;
        }
    }
    
    /* Check filename pattern */
    if (opts->file_pattern[0] && !matches_pattern(filename, opts->file_pattern)) {
        fclose(file);
        return 0;
    }
    
    stats->files_searched++;
    
    int match_in_file = 0;
    int line_number = 0;
    char line[MAX_LINE];
    
    /* Search in content */
    if (opts->search_content) {
        while (fgets(line, sizeof(line), file)) {
            line_number++;
            
            /* Remove newline */
            line[strcspn(line, "\n")] = '\0';
            
            /* Check each keyword */
            for (int k = 0; k < opts->keyword_count; k++) {
                const char *found;
                
                if (opts->case_sensitive) {
                    found = strstr(line, opts->keywords[k]);
                } else {
                    found = strstr_case(line, opts->keywords[k]);
                }
                
                if (found) {
                    stats->total_matches++;
                    match_in_file = 1;
                    
                    if (!opts->count_only && !opts->only_matching_files) {
                        if (opts->show_line_numbers) {
                            printf("%s:%d:%s\n", filename, line_number, line);
                        } else {
                            printf("%s:%s\n", filename, line);
                        }
                    }
                    
                    if (opts->only_matching_files) {
                        /* Found at least one match */
                        fclose(file);
                        stats->files_matched++;
                        return 1;
                    }
                }
            }
        }
    }
    
    fclose(file);
    
    /* Search in filename */
    if (opts->search_filenames && !match_in_file) {
        const char *basename = strrchr(filename, '/');
        basename = basename ? basename + 1 : filename;
        
        for (int k = 0; k < opts->keyword_count; k++) {
            const char *found;
            
            if (opts->case_sensitive) {
                found = strstr(basename, opts->keywords[k]);
            } else {
                found = strstr_case(basename, opts->keywords[k]);
            }
            
            if (found) {
                stats->total_matches++;
                match_in_file = 1;
                stats->files_matched++;
                
                if (!opts->count_only) {
                    printf("Filename match: %s\n", filename);
                }
                return 1;
            }
        }
    }
    
    if (match_in_file) {
        stats->files_matched++;
        if (opts->only_matching_files && !opts->count_only) {
            printf("%s\n", filename);
        }
    }
    
    return match_in_file;
}

/* Recursively search directory SAFELY */
void search_directory(const char *path, int depth, 
                     const SearchOptions *opts, SearchStats *stats) {
    if (!path) return;
    
    /* Check depth limit */
    if (opts->max_depth >= 0 && depth > opts->max_depth) {
        return;
    }
    
    DIR *dir = opendir(path);
    if (!dir) {
        return;
    }
    
    struct dirent *entry;
    char fullpath[MAX_PATH];
    
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Build full path safely */
        if (strlen(path) + strlen(entry->d_name) + 2 > MAX_PATH) {
            continue;  /* Path too long */
        }
        
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (lstat(fullpath, &st) != 0) {
            continue;  /* Skip if can't stat */
        }
        
        /* Check if it's a directory */
        if (S_ISDIR(st.st_mode)) {
            if (opts->recursive) {
                search_directory(fullpath, depth + 1, opts, stats);
            }
        } 
        /* Check if it's a regular file */
        else if (S_ISREG(st.st_mode)) {
            search_file(fullpath, opts, stats);
        }
        /* Skip other file types (symlinks, devices, etc.) */
    }
    
    closedir(dir);
}

/* Print help message */
void print_help(void) {
    printf("File Walker - Safe recursive file search\n");
    printf("Usage: fwalker [OPTIONS] [DIRECTORY] keyword1 [keyword2 ...]\n\n");
    printf("Options:\n");
    printf("  -i            Case-insensitive search\n");
    printf("  -r            Recursive search (default: on)\n");
    printf("  -l            Only show names of files with matches\n");
    printf("  -c            Only count matches, don't show them\n");
    printf("  -n            Show line numbers\n");
    printf("  -f PATTERN    Search only files matching pattern (e.g., *.c)\n");
    printf("  -d DEPTH      Maximum directory depth (default: unlimited)\n");
    printf("  -s MIN_SIZE   Minimum file size in bytes\n");
    printf("  -S MAX_SIZE   Maximum file size in bytes\n");
    printf("  -h            Show this help message\n\n");
    printf("Examples:\n");
    printf("  fwalker error                   # Search for 'error' in current dir\n");
    printf("  fwalker /etc error              # Search /etc directory\n");
    printf("  fwalker -f \"*.c\" main        # Search 'main' in all .c files\n");
    printf("  fwalker -d 2 -s 1000 test      # Search 'test' in files >1KB\n");
}

/* Print statistics */
void print_stats(const SearchStats *stats) {
    time_t end_time = time(NULL);
    double elapsed = difftime(end_time, stats->start_time);
    
    printf("\n=== Search Statistics ===\n");
    printf("Files searched:    %ld\n", stats->files_searched);
    printf("Files matched:     %ld\n", stats->files_matched);
    printf("Total matches:     %ld\n", stats->total_matches);
    printf("Total size:        %ld bytes\n", stats->total_size);
    printf("Time elapsed:      %.2f seconds\n", elapsed);
    
    if (stats->files_searched > 0) {
        printf("Avg file size:     %.2f KB\n", 
               (double)stats->total_size / stats->files_searched / 1024);
        if (stats->total_matches > 0) {
            printf("Matches per file:  %.2f\n", 
                   (double)stats->total_matches / stats->files_searched);
        }
    }
}

/* Main function - safe entry point */
int main(int argc, char *argv[]) {
    SearchOptions opts;
    SearchStats stats = {0};
    
    if (argc < 2) {
        print_help();
        return EXIT_FAILURE;
    }
    
    /* Initialize everything */
    init_options(&opts);
    parse_arguments(argc, argv, &opts);
    
    printf("Searching for: ");
    for (int i = 0; i < opts.keyword_count; i++) {
        printf("\"%s\" ", opts.keywords[i]);
    }
    printf("\n");
    
    printf("Starting directory: %s\n", opts.start_dir);
    printf("Case %s\n", opts.case_sensitive ? "sensitive" : "insensitive");
    
    if (opts.file_pattern[0]) {
        printf("File pattern: %s\n", opts.file_pattern);
    }
    
    printf("----------------------------------------\n");
    
    stats.start_time = time(NULL);
    
    /* Start search from specified directory */
    search_directory(opts.start_dir, 0, &opts, &stats);
    
    if (!opts.count_only && !opts.only_matching_files) {
        printf("\n");
    }
    
    print_stats(&stats);
    
    return EXIT_SUCCESS;
}
