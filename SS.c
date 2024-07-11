#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>

// So, the storage server is basically a client to the Naming Server,
// and a server to the clients.

#define MAX_CLIENTS 100
#define MAX_CHARS 100
#define MAX_FILE_LENGTH 999999
#define MAX_PATHS 20
#define ALPHABET_SIZE (128)
#define CHUNK_SIZE 16
#define PORT_NM 5000 // NM's port for SS to connect
// #define PORT_NM_REV 4554 // SS' port for NM to connect
#define ALT_PORT_NM 5050 // try alt. port just in case PORT_NM doesn't work

struct SS_INFO
{
    char ip_addr[30];
    int port_nm;
    int port_cln;
    int no_paths;
    int sockid;
    char accessible_paths[100][100];
};
typedef struct SS_INFO *SS_INFO;

struct TrieNode
{
    struct TrieNode *children[ALPHABET_SIZE];

    sem_t rw_queue;
    sem_t write_lock;

    // isEndOfWord is true if the node represents
    // end of a word
    bool isEndOfWord;
    bool isDir;
    int readers;
};

struct CLIENT
{
    int type;
    int sockid;
    struct sockaddr_in addr;
};
typedef struct CLIENT *CLIENT;

struct Response
{
    int responseCode;
    char *responseBuffer;
};
typedef struct Response *Response;

struct TrieNode *getNode(void);
void insert(struct TrieNode *root, const char *key);
struct TrieNode *GetTrieNode(struct TrieNode *root, const char *key);
void delete(struct TrieNode *root, const char *key);
bool isNodeEmpty(struct TrieNode *root);

struct TrieNode *trie_root;
sem_t trie_lock;

// NM OPERATIONS
int CreateFileDirectory(char *token, char *return_buffer)
{
    // for now
    // - we expect that we don't get existing files as input
    //

    if (strchr(token, '.'))
    {
        int fileDescriptor = open(token, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fileDescriptor == -1)
        {
            perror("Error opening file");
            return -1;
        }
        strcpy(return_buffer, "[200] File created Successfully!\n");
    }
    else
    {
        if (mkdir(token, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0)
        {
            perror("Error creating directory");
            return -1;
        }
        strcpy(return_buffer, "[200] Directory created Successfully!\n");
    }
    insert(trie_root, token);
    return 0;
}

int DeleteFileDirectory(char *token, char *return_buffer)
{
    // we expect
    //

    struct TrieNode *node = GetTrieNode(trie_root, token);
    sem_wait(&node->rw_queue);
    if (strchr(token, '.'))
    {
        if (remove(token) != 0)
        {
            perror("Error deleting file");
            return -1;
        }
        strcpy(return_buffer, "Deleted file successfully!\n");
    }
    else
    {
        if (rmdir(token) != 0)
        {
            perror("Error removing directory");
            return 1;
        }
        strcpy(return_buffer, "Directory removed successfully!\n");
    }
    sem_post(&node->rw_queue);

    delete (trie_root, token);
}

const char *stop_packet = "<STOP>";
void sendChunks(int sockid, char *buffer)
{
    int buffer_length = strlen(buffer) + 1;
    int chunks = buffer_length / CHUNK_SIZE;
    int mod_chunk = buffer_length % CHUNK_SIZE;

    int i = 0;
    while (i <= chunks)
    {
        // printf("%s\n", &buffer[i * CHUNK_SIZE]);
        send(sockid, &buffer[i * CHUNK_SIZE], CHUNK_SIZE, 0);
        i++;
    }

    // sending the stop packet
    send(sockid, stop_packet, CHUNK_SIZE, 0);
}

void receiveChunks(int sockid, char *recieve_buffer)
{
    // char buffer[CHUNK_SIZE];
    int num_chunks = 0;
    while (1)
    {
        int n = recv(sockid, &recieve_buffer[num_chunks * CHUNK_SIZE], CHUNK_SIZE, 0);
        if (n < 0)
        {
            perror("[-]Recv error");
            exit(1);
        }
        if (strcmp(&recieve_buffer[num_chunks * CHUNK_SIZE], stop_packet) == 0)
            break;
        num_chunks++;
    }
    // printf("recv: %s\n", recieve_buffer);
}

// CLIENT OPERATIONS
int ReadFile(int sockid, char *token, char *return_buffer)
{
    int file_descriptor = open(token, O_RDONLY);
    if (file_descriptor == -1)
    {
        perror("open");
        strcpy(return_buffer, "[400] File unavailable\n");
        return -1;
    }

    struct TrieNode *node = GetTrieNode(trie_root, token);

    char buffer[CHUNK_SIZE];
    int i = 0;

    int present;
    sem_getvalue(&node->write_lock, &present);
    if(!present && node->readers == 0){
        strcpy(return_buffer, "[302] Some user is writing the file - Unable to read\nTry again after some time :(\n");
        return 301;
    }

    if(node->readers == 0)
        sem_wait(&node->write_lock);
    node->readers++;
    ssize_t bytes_read;
    do
    {
        bytes_read = read(file_descriptor, buffer, CHUNK_SIZE);

        if (bytes_read == -1)
        {
            perror("read");
            strcpy(return_buffer, "[500] Error reading file\n");
            close(file_descriptor);
            return -1;
        }
        buffer[bytes_read] = '\0';

        send(sockid, buffer, CHUNK_SIZE, 0);
        // sleep(1);
        i++;
    }
    while (bytes_read > 0);
    node->readers--;

    if(node->readers == 0)
        sem_post(&node->write_lock);

    close(file_descriptor);

    // sending the stop packet
    send(sockid, stop_packet, CHUNK_SIZE, 0);
    return 0;
}

int WriteFile(int sockid, char *token, char *return_buffer)
{
    // first token - "Info to be written"
    // second token - path
    char write_buffer[MAX_FILE_LENGTH];

    char *file_name = strdup(token);
    struct TrieNode *node = GetTrieNode(trie_root, file_name);
    int present;
    sem_getvalue(&node->write_lock, &present);
    if(!present){
        strcpy(return_buffer, "[301] Some user is reading the file - Unable to write\nTry again after some time\n");
        return 301;
    }

    int fileDescriptor = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fileDescriptor == -1)
    {
        perror("Error opening file");
        strcpy(return_buffer, "[404] Error opening the file\n");
        return -1;
    }

    token = strtok(NULL, "\t");
    if (token == NULL)
    {
        strcpy(return_buffer, "[-01] Content for write not provided\n$ WRITE\t[path]\t[content]\n");
        return -1;
    }
    strcpy(write_buffer, token);

    // sem_wait(&node->rw_queue);
    
    sem_wait(&node->write_lock);

    ssize_t bytes_written = write(fileDescriptor, write_buffer, strlen(write_buffer));

    if (bytes_written == -1)
    {
        perror("Error writing to file");
        strcpy(return_buffer, "[500] Error writing to the file\n");
        close(fileDescriptor);
        return -1;
    }

    strcpy(return_buffer, "Successfully written to file\n");

    sem_post(&node->write_lock);
    // sem_post(&node->rw_queue);

    close(fileDescriptor);
    return 0;
}

int GetSizeAndPermissions(char *token, char *return_buffer)
{
    struct TrieNode *node = GetTrieNode(trie_root, token);

    sem_wait(&node->rw_queue);
    struct stat file_info;
    if (stat(token, &file_info) == -1)
    {
        strcpy(return_buffer, "Error getting file information\n");
        perror("Error getting file information");
        sem_post(&node->rw_queue);
        return -1;
    }
    sem_post(&node->rw_queue);

    snprintf(return_buffer, MAX_CHARS * 2,
             "File Size: %lld bytes\nFile Permissions: %o\n",
             (long long)file_info.st_size,
             file_info.st_mode & 0777);

    return 0;
}

int getFilesInDir(char *token, char *return_buffer)
{

    strcpy(return_buffer, "");

    // Open the directory
    DIR *dir = opendir(token);
    if (dir == NULL)
    {
        strcpy(return_buffer, "Error opening directory\n");
        perror("Error opening directory");
        exit(EXIT_FAILURE);
    }

    // Read the directory entries
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        // Skip "." and ".." entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        strcat(return_buffer, entry->d_name);
        strcat(return_buffer, ",");
    }
    return_buffer[strlen(return_buffer) - 1] = '\0';
    printf("%s\n", return_buffer);
    closedir(dir);
}

// int GetFile(int sockid, char *token, char *return_buffer)
// {
//     if (ReadFile(sockid, token, return_buffer) == 0)
//     {
//         sendChunks(sockid, return_buffer);
//         return 0;
//     }
//     strcpy(return_buffer, "Reading success.\n");
//     return -1;
// }

char buffer[CHUNK_SIZE];
int PutFile(int sockid, char *token, char *return_buffer)
{
    struct TrieNode *node = GetTrieNode(trie_root, token);
    char *file_name = strdup(token);
    int fileDescriptor = open(file_name, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fileDescriptor == -1)
    {
        perror("Error opening file");
        strcpy(return_buffer, "[404] Error opening the file\n");
        return -1;
    }


    int present;
    sem_getvalue(&node->write_lock, &present);
    if(!present){
        strcpy(return_buffer, "[301] Some user is reading the file - Unable to write\nTry again after some time\n");
        return 301;
    }

    sem_wait(&node->write_lock);
    int num_chunks = 0;
    while (1)
    {
        int n = recv(sockid, buffer, CHUNK_SIZE, 0);
        if (n < 0)
        {
            perror("[-]Recv error");
            exit(1);
        }
        if (strcmp(buffer, stop_packet) == 0)
            break;
        else
        {
            sem_wait(&node->rw_queue);
            ssize_t bytes_written = write(fileDescriptor, buffer, strlen(buffer) );
            sem_post(&node->rw_queue);

            if (bytes_written == -1)
            {
                perror("Error writing to file");
                strcpy(return_buffer, "[500] Error writing to the file\n");
                close(fileDescriptor);
                sem_post(&node->rw_queue);
                sem_post(&node->write_lock);
                return -1;
            }
        }
        num_chunks++;
    }
    close(fileDescriptor);
    sem_post(&node->write_lock);

    strcpy(return_buffer, "PUT: Content written success..\n");
    return 0;
}

void *client_handler(void *arg);
void *naming_handler(void *arg);

int main(int argc, char *argv[])
{
    int port_cln = atoi(argv[1]);
    int PORT_NM_REV = atoi(argv[2]);

    char *serv_ip = "127.0.0.1";
    char *nm_ip = "127.0.0.1";

    SS_INFO ss_info = (SS_INFO)malloc(sizeof(struct SS_INFO));
    strcpy(ss_info->ip_addr, serv_ip);
    ss_info->port_cln = port_cln;
    ss_info->port_nm = PORT_NM_REV;

    trie_root = getNode();
    sem_init(&trie_lock, 0, 1);

    int N;
    printf("No. of accessible paths: ");
    scanf("%d", &N);
    ss_info->no_paths = N;

    char paths[MAX_PATHS][100];
    for (int i = 0; i < N; i++)
    {
        scanf("%s", paths[i]);
        insert(trie_root, paths[i]);
        strcpy(ss_info->accessible_paths[i], paths[i]);
    }

    int nm_sock, ss_sock;
    struct sockaddr_in nm_addr, ss_addr;
    socklen_t addr_size;

    // initializing sockets
    ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    ss_info->sockid = ss_sock;

    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0)
    {
        perror("[-]Socket error");
        exit(1);
    }
    printf("* | Created Storage Server's socket on port %d.\n", PORT_NM);

    // socket port for NM
    memset(&nm_addr, '\0', sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = PORT_NM;
    nm_addr.sin_addr.s_addr = inet_addr(nm_ip);

    // we use connect here, because it is a client to the naming server
    int conn = connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr));
    if (conn == -1)
    {
        nm_addr.sin_port = ALT_PORT_NM;
        conn = connect(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr));
    }
    printf("Connected to the Naming Server at port %d status: %d\n", PORT_NM, conn);

    char* ss = "SS";
    send(nm_sock, ss, MAX_CHARS, 0);
    send(nm_sock, &ss_info->no_paths, sizeof(int), 0);
    send(nm_sock, ss_info, sizeof(struct SS_INFO), 0);

    // closing the socket and binding it with a port
    close(nm_sock);

    // for binding of the NM_REV
    nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    nm_addr.sin_port = PORT_NM_REV;
    nm_addr.sin_addr.s_addr = inet_addr(serv_ip);

    if (bind(nm_sock, (struct sockaddr *)&nm_addr, sizeof(nm_addr)) < 0)
    {
        perror("[-] Bind error");
    }
    printf("* | Bound the NM socket to port: %d \n", nm_addr.sin_port);

    listen(nm_sock, MAX_CLIENTS);

    int nm_sockid = nm_sock;
    pthread_t naming_thread;
    pthread_create(&naming_thread, NULL, naming_handler, &nm_sockid);

    // socket port for Clients
    memset(&ss_addr, '\0', sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = port_cln;
    ss_addr.sin_addr.s_addr = inet_addr(serv_ip);

    // we bind here, to open a server for the clients to connect()
    if (bind(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
    {
        perror("[-]Bind error");
    }
    printf("* | Bind to the port_cln number: %d\n", port_cln);

    listen(ss_sock, MAX_CLIENTS);
    printf("* | Listening... \n");

    pthread_t threads[MAX_CLIENTS];
    int no_clients = 0;

    while (1)
    {
        CLIENT client = (CLIENT)malloc(sizeof(struct CLIENT));
        addr_size = sizeof(client->addr);

        // accepting clients, then creating threads for each
        client->sockid = accept(ss_sock, (struct sockaddr *)&client->addr, &addr_size);
        client->type = 0;
        // printf("HELLO");

        pthread_create(&threads[no_clients++], NULL, client_handler, client);
    }

    return 0;
}

void *client_handler(void *arg)
{
    CLIENT client = (CLIENT)arg;
    printf("[+]Client connected. %d %d\n", client->sockid, client->addr.sin_port);

    char buffer[MAX_CHARS];

    // receiving the command from the client.
    bzero(buffer, MAX_CHARS);

    int n = recv(client->sockid, buffer, sizeof(buffer), 0);
    if (n == -1)
    {
        perror("recv");
    }
    printf("Client: %s\n", buffer);

    char return_buffer[MAX_FILE_LENGTH];

    printf("HELLO\n");

    char path[MAX_CHARS];
    strcpy(path, ".");

    char *operation = strtok(buffer, "\t\n");
    if(strcmp(operation, "CHECK") == 0){
        // 
        return NULL;
    }
    char *def_path = strtok(NULL, "\t\n");
    if (def_path == NULL)
    {
        strcpy(return_buffer, "Path not defined :(\n");
        sendChunks(client->sockid, return_buffer);
    }

    if (def_path[0] == '/')
        strcat(path, def_path);
    else
        strcpy(path, def_path);

    if (client->type)
    {
        if (strcmp(operation, "CREATE") == 0)
        {
            int response = CreateFileDirectory(path, return_buffer);
        }
        else if (strcmp(operation, "DELETE") == 0)
        {
            int response = DeleteFileDirectory(path, return_buffer);
        }
        else if (strcmp(operation, "GIVEFILES") == 0)
        {
            int response = getFilesInDir(path, return_buffer);
        }
        else if (strcmp(operation, "GET") == 0)
        {
            int response = ReadFile(client->sockid, path, return_buffer);
            printf("ret: %s\n", return_buffer);
        }
        else if (strcmp(operation, "PUT") == 0)
        {
            int response = PutFile(client->sockid, path, return_buffer);
        }
        sendChunks(client->sockid, return_buffer);
    }
    else
    {
        if (strcmp(operation, "READ") == 0)
        {
            int response = ReadFile(client->sockid, path, return_buffer);
        }
        else if (strcmp(operation, "WRITE") == 0)
        {
            int response = WriteFile(client->sockid, path, return_buffer);
        }
        else if (strcmp(operation, "INFO") == 0)
        {
            int response = GetSizeAndPermissions(path, return_buffer);
        }
        sendChunks(client->sockid, return_buffer);
    }

    return NULL;
}

void *naming_handler(void *arg)
{
    int nm_sock = *(int *)arg;

    while (1)
    {
        CLIENT NM = (CLIENT)malloc(sizeof(struct CLIENT));
        socklen_t addr_size = sizeof(NM->addr);
        NM->type = 1;

        // accepting clients, then creating threads for each
        NM->sockid = accept(nm_sock, (struct sockaddr *)&NM->addr, &addr_size);
        printf("[+] NM connected. %d\n", nm_sock);

        pthread_t nm_handler;
        pthread_create(&nm_handler, NULL, client_handler, NM);
    }
}

struct TrieNode *getNode(void)
{
    struct TrieNode *pNode = NULL;

    pNode = (struct TrieNode *)malloc(sizeof(struct TrieNode));

    if (pNode)
    {
        int i;

        pNode->isDir = true;
        pNode->isEndOfWord = false;

        for (i = 0; i < ALPHABET_SIZE; i++)
            pNode->children[i] = NULL;
    }

    return pNode;
}

void insert(struct TrieNode *root, const char *key)
{
    sem_wait(&trie_lock);
    int level;
    int length = strlen(key);
    int index;

    struct TrieNode *pCrawl = root;

    for (level = 0; level < length; level++)
    {
        index = key[level];
        if (!pCrawl->children[index])
            pCrawl->children[index] = getNode();

        pCrawl = pCrawl->children[index];
    }

    sem_init(&pCrawl->rw_queue, 0, 1);
    sem_init(&pCrawl->write_lock, 0, 1);
    pCrawl->readers = 0;

    if (strchr(key, '.'))
        pCrawl->isDir = false;

    // mark last node as leaf
    pCrawl->isEndOfWord = true;
    sem_post(&trie_lock);
}

struct TrieNode *GetTrieNode(struct TrieNode *root, const char *key)
{
    sem_wait(&trie_lock);
    int level;
    int length = strlen(key);
    int index;
    struct TrieNode *pCrawl = root;

    for (level = 0; level < length; level++)
    {
        index = key[level];

        if (!pCrawl->children[index])
        {
            sem_post(&trie_lock);
            insert(pCrawl, &key[level]);
            sem_wait(&trie_lock);
        }

        pCrawl = pCrawl->children[index];
        printf("%c", key[level]);
    }
    printf("\n");
    sem_post(&trie_lock);
    return (pCrawl);
}

bool deleteHelper(struct TrieNode *root, const char *key, int level, int length)
{
    if (root)
    {
        // Base case: If the last character of the key is being processed
        if (level == length)
        {
            if (root->isEndOfWord)
            {
                // Unmark the end of the word flag
                root->isEndOfWord = false;

                // If the node has no other children, it is safe to delete
                return isNodeEmpty(root);
            }
        }
        else // Recursive case: Traverse to the next level
        {
            int index = key[level];
            if (deleteHelper(root->children[index], key, level + 1, length))
            {
                // Delete the node if it has no children and not marked as the end of a word
                if (!isNodeEmpty(root->children[index]) && !root->children[index]->isEndOfWord)
                {
                    free(root->children[index]);
                    root->children[index] = NULL;

                    // If the current node has no other children, it is safe to delete
                    return isNodeEmpty(root);
                }
            }
        }
    }

    return false;
}

// Delete a key from the trie
void delete(struct TrieNode *root, const char *key)
{
    sem_wait(&trie_lock);
    int length = strlen(key);
    deleteHelper(root, key, 0, length);
    sem_post(&trie_lock);
}

bool isNodeEmpty(struct TrieNode *root)
{
    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (root->children[i] != NULL)
            return false;
    }
    return true;
}
