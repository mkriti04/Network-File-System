#include <stdbool.h>
 
// Alphabet size (# of symbols)
#define ALPHABET_SIZE (128)
 
// trie node
struct TrieNode
{
    struct TrieNode *children[ALPHABET_SIZE];
    
    sem_t rw_queue;
    sem_t write_lock;

    // isEndOfWord is true if the node represents
    // end of a word
    bool isEndOfWord;
    bool isDir;
};
 
// Returns new trie node (initialized to NULLs)
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
 
// If not present, inserts key into trie
// If the key is prefix of trie node, just marks leaf node
void insert(struct TrieNode *root, const char *key)
{
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

    if(strchr(key, '.'))
        pCrawl->isDir = false;

    // mark last node as leaf
    pCrawl->isEndOfWord = true;
}
 
struct TrieNode* GetTrieNode(struct TrieNode *root, const char *key)
{
    int level;
    int length = strlen(key);
    int index;
    struct TrieNode *pCrawl = root;
    struct TrieNode *parent = pCrawl;

    for (level = 0; level < length; level++)
    {
        index = key[level];
 
        if (!pCrawl->children[index])
            insert(parent, key + level);
 
        parent = pCrawl;
        pCrawl = pCrawl->children[index];
        printf("%c", key[level]);
    }
    printf("\n");
 
    return (pCrawl);
}

// Driver
int main()
{
    // Input keys (use only 'a' through 'z' and lower case)
    char keys[][8] = {"the", "a", "there", "answer", "any",
                     "by", "bye", "their"};
 
    char output[][32] = {"Not present in trie", "Present in trie"};
 
 
    struct TrieNode *root = getNode();
 
    // Construct trie
    int i;
    for (i = 0; i < ARRAY_SIZE(keys); i++)
        insert(root, keys[i]);
 
    struct TrieNode* node = GetTrieNode(root, "directory/file1.txt");
 
    return 0;
}
