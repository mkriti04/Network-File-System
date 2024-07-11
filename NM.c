#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "structs.h"
#define LOG_FILE "log.txt"
#define MAX_LOG_MSG_SIZE 256

sem_t x, y;
ss_info *ss_array;
ss_backup_info *backup_ss_array;
pthread_t ss_thread[100];
pthread_t client_thread[100];
struct LRUcache *lruQueue;
sem_t LRU_lock;
FILE *logFile;
int ss_num = 0;
int c_num = 0;

struct TrieNode *root;
void logMessage(const char *message, int socket, int status)
{
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    if (getpeername(socket, (struct sockaddr *)&addr, &addrlen) == 0)
    {
        char ipAddr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ipAddr, INET_ADDRSTRLEN);

        char logMsg[MAX_LOG_MSG_SIZE];
        int msgLength = snprintf(logMsg, MAX_LOG_MSG_SIZE, "[%s:%d] %s - Status: %d\n", ipAddr, ntohs(addr.sin_port), message, status);

        int logFile = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
        if (logFile == -1)
        {
            perror("Error opening log file");
            exit(EXIT_FAILURE);
        }

        if (write(logFile, logMsg, msgLength) == -1)
        {
            perror("Error writing to log file");
        }

        close(logFile);
    }
    else
    {
        perror("getpeername");
    }
}

int COPY(char *path1, char *path2)
{
    ss_info *ss_struct_1 = getFromLRUcache(lruQueue, path1);
    if (ss_struct_1 == NULL)
    {
        ss_struct_1 = search(root, path1);
        enqueue(lruQueue, path1, ss_struct_1);
    }
    if (ss_struct_1 == NULL)
    {
        perror("File/Directory unavailable");
        return 404;
    }

    printf("1\n");
    // SS for path2
    ss_info *ss_struct_2 = getFromLRUcache(lruQueue, path2);
    if (ss_struct_2 == NULL)
    {
        ss_struct_2 = search(root, path2);
        enqueue(lruQueue, path2, ss_struct_2);
    }
    if (ss_struct_2 == NULL)
    {
        perror("File/Directory unavailable");
        return 404;
    }

    printf("2\n");

    char filename[20];
    char *token = strdup(path1);
    token = strtok(token, "/");
    while (token != NULL)
    {
        strcpy(filename, token);
        token = strtok(NULL, "/");
    }
    strcat(path2, filename);
    printf("New file path: %s\n", path2);

    // for file
    int sender_sock = reconnectToSS(ss_struct_1);
    int receiver_sock = reconnectToSS(ss_struct_2);
    printf("Sockets created");

    char *m = strchr(filename, '.');
    // printf("%d\n", m-filename);
    if (m)
    {
        copyHelper(sender_sock, receiver_sock, path1, path2);
    }
    // for directory
    else
    {
        printf("hello DIR\n");
        if(path2[strlen(path2) - 1] != '/')
            strcat(path2, "/");
        copyDir(ss_struct_1, ss_struct_2, sender_sock, receiver_sock, path1, path2);
    }
}

int search_for_ss(int port){
    // if found, returns 1, else 0
    return -1;
}

int tryConnect(ss_info* ss_struct){
    int ss_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_socket < 0)
    {
        perror("[-]Socket error");
        return 0;
    }
    struct sockaddr_in ss_addr;
    memset(&ss_addr, '\0', sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = ss_struct->port_no_ns;
    printf("re: %d %d\n", ss_struct->port_no_client, ss_struct->port_no_ns);
    ss_addr.sin_addr.s_addr = inet_addr(ss_struct->ip_addr);
    int n = connect(ss_socket, (struct sockaddr *)&ss_addr, sizeof(ss_addr));
    if (n < 0)
    {
        // perror("[-]Connection error");
        close(ss_socket);
        return 0;
    }
    char buffer[MAX_CHARS];
    strcpy(buffer, "CHECK");
    n = send(ss_socket, buffer, MAX_CHARS, 0);
    close(ss_socket);
    return 1;
}

void *ss_handler(void *param)
{
    int socket = *(int *) param;
    // sem_wait(&x);
    int num;
    int m = recv(socket, &num, sizeof(int), 0);
    if (m == -1)
    {
        perror("[-]receive error45");
        exit(1);
    }

    int n = recv(socket, &ss_array[ss_num], sizeof(ss_info), 0);
    if (n == -1)
    {
        perror("[-]receive error44");
        exit(1);
    }
    close(socket);
    
    printf("SAD2F\n");
    
    int found = 0;
    int backups[] = {-1,-1};
    int ss_index = search_for_ss(ss_array[ss_num].port_no_ns);
    if(ss_index == -1 || backup_ss_array[ss_index].has_dup == 0){
        // have to search for existing storage servers to duplicate into
        for(int i = ss_num - 1; i >= 0; i--){
            if(ss_array[i].port_no_ns != ss_array[ss_num].port_no_ns){
                if(tryConnect(&ss_array[i])){
                    backups[found++] = i;
                    if(found == 2){
                        break;
                    }
                }
            }
        }

        printf("SApoooF %d\n", found);
        if(ss_index == -1)
            ss_index = ss_num;
        if(found == 2){
            backup_ss_array[ss_index].has_dup = 1;
            strcpy(backup_ss_array[ss_index].ip_addr, ss_array[ss_index].ip_addr);
            backup_ss_array[ss_index].port_no_ns_b1 = ss_array[backups[0]].port_no_ns;
            backup_ss_array[ss_index].port_no_ns_b2 = ss_array[backups[1]].port_no_ns;
            backup_ss_array[ss_index].port_no_client_b1 = ss_array[backups[0]].port_no_client;
            backup_ss_array[ss_index].port_no_client_b2 = ss_array[backups[1]].port_no_client;
            
            char buffer[MAX_CHARS];
            int socket1 = reconnectToSS(&ss_array[backups[0]]);
            strcpy(buffer, "backup/");
            FILE_("CREATE\t", socket1, buffer);
            snprintf(buffer, sizeof(buffer), "backup/SS%d_1/", ss_array[ss_index].port_no_ns);
            socket1 = reconnectToSS(&ss_array[backups[0]]);
            FILE_("CREATE\t", socket1, buffer);

            int n = close(socket1);
            if (n < 0)
            {
                perror("close error");
                exit(1);
            }

            int socket2 = reconnectToSS(&ss_array[backups[1]]);
            strcpy(buffer, "backup/");
            FILE_("CREATE\t", socket2, buffer);
            snprintf(buffer, sizeof(buffer), "backup/SS%d_2/", ss_array[ss_index].port_no_ns);
            socket2 = reconnectToSS(&ss_array[backups[1]]);
            FILE_("CREATE\t", socket2, buffer);
            printf("helllo2\n");    

            n = close(socket2);
            if (n < 0)
            {
                perror("close error");
                exit(1);
            }
            printf("LOKKKK\n");
        }
        else{
            // take lite
        }
    }

    printf("SADFFSF\n");

    for (int k = 0; k < ss_array[ss_num].no_acc_paths; k++)
    {
        char buffer[1024];
        printf("%s\n", ss_array[ss_num].accesible_paths[k]);
        insert(root, ss_array[ss_num].accesible_paths[k], &ss_array[ss_num]);
        if(found == 2){
            ss_num++;
            
            ss_array = (ss_info*) realloc(ss_array, sizeof(ss_info) * (ss_num + 1));
            backup_ss_array = (ss_backup_info*) realloc(backup_ss_array, sizeof(ss_backup_info) * (ss_num + 1));

            snprintf(buffer, sizeof(buffer), "backup/SS%d_1/%s", ss_array[ss_index].port_no_ns, ss_array[ss_index].accesible_paths[k]);
            insert(root, buffer, &ss_array[backups[0]]);

            snprintf(buffer, sizeof(buffer), "backup/SS%d_1/", ss_array[ss_index].port_no_ns);
            insert(root, buffer, &ss_array[backups[0]]);
            COPY(ss_array[ss_index].accesible_paths[k], buffer);
            
            snprintf(buffer, sizeof(buffer), "backup/SS%d_2/%s", ss_array[ss_index].port_no_ns, ss_array[ss_index].accesible_paths[k]);
            insert(root, buffer, &ss_array[backups[1]]);
            
            snprintf(buffer, sizeof(buffer), "backup/SS%d_2/", ss_array[ss_index].port_no_ns);
            insert(root, buffer, &ss_array[backups[1]]);
            COPY(ss_array[ss_index].accesible_paths[k], buffer);
        }
    }
    backup_ss_array[ss_index].has_dup = 1;
    // sem_post(&x);
}

void *client_handler(void *param)
{
    int socket = *(int *)param;
    // sem_wait(&x);

    char buffer[MAX_CHARS];
    while (1)
    {
        bzero(buffer, MAX_CHARS);
        int n = recv(socket, buffer, sizeof(buffer), 0);
        if (n == -1)
        {
            perror("[-]receive error00");
            // exit(1);
        }
        // printf(">>%s\n", buffer);
        char input[100];
        strcpy(input,buffer);
        int resCode = execute(buffer, &socket, lruQueue, root);
        if (resCode == 404)
        {
            strcpy(buffer, "[404] File / Directory Not found\n");
            logMessage(input, socket, 0);
            sendChunks(socket, buffer);
        }
        else if (resCode == -1)
        {
            strcpy(buffer, "[500] Server error: can't execute `send`\n");
            logMessage(input, socket, 0);
            sendChunks(socket, buffer);
        }
        else
        {
            logMessage(input, socket, 1);
        }
        // should receive ack from server and send the msg to client
    }

    pthread_exit(NULL);
}

int main()
{
    root = getNode();

    // have to change *** POSIX ***
    logFile = fopen("log.txt", "w");
    if (logFile == NULL)
    {
        perror("[-] Log file opening error");
        exit(1);
    }
    int n, N;
    scanf("%d", &N);
    ss_array = malloc(sizeof(ss_info) * (N + 1));
    backup_ss_array = malloc(sizeof(ss_backup_info) * (N + 1));
    char *ip = "127.0.0.1";
    int port = 5000;
    lruQueue = initLRUcache(10);

    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_size;
    char buffer[MAX_CHARS];

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0)
    {
        perror("[-]Socket error");
        exit(1);
    }
    printf("[+]TCP server socket created.\n");

    memset(&server_addr, '\0', sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = port;
    server_addr.sin_addr.s_addr = inet_addr(ip);

    n = bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (n < 0)
    {
        server_addr.sin_port = 5050;
        n = bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        perror("[-]Bind error");
        // exit(1);
    }
    printf("[+]Bind to the port number: %d\n", port);

    n = listen(server_sock, 5);
    if (n == -1)
    {
        perror("[-]listen error");
        exit(1);
    }
    printf("Listening...\n");

    for (int i = 0; i < N; i++)
    {
        backup_ss_array[i].has_dup = 0;
        addr_size = sizeof(client_addr);
        client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_size);
        if (client_sock == -1)
        {
            perror("[-]accept");
            exit(1);
        }
        ss_num++;
        bzero(buffer, MAX_CHARS);
        // printf("S<%d>\n", client_sock);
        n = recv(client_sock, buffer, sizeof(buffer), 0);
        if (n == -1)
        {
            perror("[-]receive error99");
            exit(1);
        }
        if (pthread_create(&ss_thread[i], NULL, ss_handler, &client_sock) != 0)
        {
            printf("Failed to create thread\n");
        }
    }

    while (1)
    {
        int client_socket;
        struct sockaddr_in client_address;
        addr_size = sizeof(client_address);
        client_socket = accept(server_sock, (struct sockaddr *)&client_address, &addr_size);
        if (client_socket == -1)
        {
            perror("[-]accept");
            exit(1);
        }
        bzero(buffer, MAX_CHARS);
        // printf("S<%d>\n", client_sock);
        n = recv(client_socket, buffer, sizeof(buffer), 0);
        if (n == -1)
        {
            perror("[-]receive error99");
            exit(1);
        }
        // printf("%s", buffer);
        if (strcmp(buffer, "SS") == 0)
        {

            if (pthread_create(&ss_thread[ss_num], NULL, ss_handler, &client_socket) != 0)
                printf("Failed to create thread\n");
        }
        else
        {
            if (pthread_create(&client_thread[c_num++], NULL, client_handler, &client_socket) != 0)
                printf("Failed to create thread\n");
        }
    }
    n = close(client_sock);
    if (n < 0)
    {
        perror("close error");
        exit(1);
    }
    for (int i = 0; i < N; i++)
    {
        pthread_join(ss_thread[i], NULL);
    }
    return 0;
}