#ifndef INDDEXCONTEXT_H__
#define INDDEXCONTEXT_H__

class IndexContext
{
    public:
        void SetupContext(const char* file_name, IndexBlockTable * p_blockTable)
        {
            m_IndexBlockTable = p_blockTable
        }
        /*
        * we could get an IndexReader by a token
        */
        IndexReader * GetReader(const uint8_t * p_token, uint32_t token_len);
        
        /*
        * Or we could get an composite Reader which specify
        * differnt tokens, which would be a tree for combinations such as:
        * "Innovative" 
        */
        IndexReader * GetReader(EvalTree);

        IndexSearchExecutor * GetExecutor();
    private:
        boost::shared_ptr<IndexBlockTable> m_IndexBlockTable;
};

#endif