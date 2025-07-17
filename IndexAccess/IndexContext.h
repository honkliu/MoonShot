#ifndef INDDEXCONTEXT_H__
#define INDDEXCONTEXT_H__

#include "AdvancedIndexReader.h"
#include "AdvancedIndexWriter.h"
#include "EvalExpression.h"
#include "IndexSearchExecutor.h"
#include "ConfigParameters.h"
#include "Tokenizer.h"
#include "Embeddings.h"
#include <memory>
#include <string>

using std::shared_ptr;
using std::make_shared;
using std::string;

class IndexContext
{
    public:
        IndexContext(const char * config_file, const char * index_file)
        {
            m_Parameters = make_shared<ConfigParameters>();
            m_IndexBlockTable = make_shared<IndexBlockTable>();

        }

        /*
        * we could get an IndexReader by a token
        */
        shared_ptr<IndexReader> GetReader(const char * p_token)
        {
            //std::shared_ptr<AdvancedIndexReader> index_reader(new AdvancedIndexReader());
            auto index_reader = make_shared<AdvancedIndexReader>();

            return index_reader;

            //return static_cast<IndexReader *>(index_reader.get());
        }
        
        /*
        * Or we could get an composite Reader which specify
        * differnt tokens, which would be a tree for combinations such as:
        * "Innovative" 
        */
        shared_ptr<IndexReader> GetReader(EvalTree *)
        {
            return nullptr;
        }

        
        /*
        * Make a member so the call would be
        * index_context->GetReader<float>() or we could ask compiler to 
        * index_context->GetReader(Embeeding<float> embeddings)
        */
        template<typename T>
        shared_ptr<IndexReader> GetReader(Embeddings<T> * embedding)
        {
            return nullptr;
        }

        shared_ptr<IndexWriter> GetWriter()
        {
            //std::shared_ptr<IndexWriter> index_writer(new IndexWriter());
            auto index_writer = make_shared<AdvancedIndexWriter>();

            return index_writer;

            //return static_cast<IndexWriter *>(index_writer.get());
        }
        /*
        shared_ptr<IndexEBWriter> GetEBWriter(const char * p_token)
        {
            //std::shared_ptr<IndexEBWriter> index_ebwriter(new IndexEBWriter());
            auto index_ebwriter = make_shared<IndexEBWriter>();

            return index_ebwriter;

            //return static_cast<IndexEBWriter *>(index_ebwriter.get());
        }*/
        IndexSearchExecutor * GetExecutor()
        {
            return NULL;
        }
    
        /*
        * Load the index into memory
        */
        void LoadIndex();
    private:
        shared_ptr<IndexBlockTable> m_IndexBlockTable;
        shared_ptr<ConfigParameters> m_Parameters;
        shared_ptr<struct IndexFile> m_IndexFile;
};

#endif