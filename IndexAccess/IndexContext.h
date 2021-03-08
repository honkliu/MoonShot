#ifndef INDDEXCONTEXT_H__
#define INDDEXCONTEXT_H__

#include "AdvancedIndexReader.h"
#include "EvalExpression.h"
#include "IndexSearchExecutor.h"
#include "ConfigParameters.h"

class IndexContext
{
    public:
        IndexContext(ConfigParameters * parameters)
        {
            m_Parameters.reset(parameters);

        }
        void SetupContext(const char* file_name, IndexBlockTable * p_blockTable)
        {
            m_IndexBlockTable.reset(p_blockTable);
        }
        /*
        * we could get an IndexReader by a token
        */
        IndexReader * GetReader(const char * p_token)
        {
            //std::shared_ptr<AdvancedIndexReader> index_reader(new AdvancedIndexReader());
            auto index_reader = std::make_shared<AdvancedIndexReader>();

            return static_cast<IndexReader *>(index_reader.get());
        }
        
        /*
        * Or we could get an composite Reader which specify
        * differnt tokens, which would be a tree for combinations such as:
        * "Innovative" 
        */
        IndexReader * GetReader(EvalTree *);

        IndexSearchExecutor * GetExecutor();
    
        /*
        * Load the index into memory
        */
        void LoadIndex();
    private:
        std::shared_ptr<IndexBlockTable> m_IndexBlockTable;
        std::shared_ptr<ConfigParameters> m_Parameters;
        std::shared_ptr<struct IndexFile> m_IndexFile;
};

#endif