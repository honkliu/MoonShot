#ifndef INDDEXCONTEXT_H__
#define INDDEXCONTEXT_H__

#include "AdvancedIndexReader.h"
#include "EvalExpression.h"
#include "IndexSearchExecutor.h"
#include "ConfigParameters.h"
#include "Embeddings.h"

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
        std::shared_ptr<IndexReader> GetReader(const char * p_token)
        {
            //std::shared_ptr<AdvancedIndexReader> index_reader(new AdvancedIndexReader());
            auto index_reader = std::make_shared<AdvancedIndexReader>();

            return index_reader;

            //return static_cast<IndexReader *>(index_reader.get());
        }
        
        /*
        * Or we could get an composite Reader which specify
        * differnt tokens, which would be a tree for combinations such as:
        * "Innovative" 
        */
        std::shared_ptr<IndexReader> GetReader(EvalTree *)
        {
            return nullptr;
        }

        /*
        * Make a member so the call would be
        * index_context->GetReader<float>() or we could ask compiler to 
        * index_context->GetReader(Embeeding<float> embeddings)
        */
        template<typename T>
        std::shared_ptr<IndexReader> GetReader(Embeddings<T> * embedding)
        {
            return nullptr;
        }

        IndexSearchExecutor * GetExecutor()
        {
            return NULL;
        }
    
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