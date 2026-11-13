# LangChain and LlamaIndex

The SDK ships vector store adapters. Install the framework yourself; `arkivector` does not depend on it.

## LangChain

```bash
pip install langchain-core
```

```python
from arkivector import Client
from arkivector.langchain import ArkivectorVectorStore

store = ArkivectorVectorStore(client=Client(), collection="notes", embedding=my_embeddings)

store.add_texts(["Cats purr.", "Dogs bark."], metadatas=[{"topic": "pets"}, {"topic": "pets"}])
docs = store.similarity_search("which animal purrs?", k=1)
docs_and_scores = store.similarity_search_with_score("which animal purrs?", k=2)
retriever = store.as_retriever(search_kwargs={"k": 4})
```

`embedding` is any LangChain `Embeddings`. Texts become entity documents, metadata becomes attributes. The collection must exist with the embedding model's dimension.

## LlamaIndex

```bash
pip install llama-index-core
```

```python
from arkivector import Client
from arkivector.llama_index import ArkivectorVectorStore

vector_store = ArkivectorVectorStore(client=Client(), collection="notes")
```

Use it as any LlamaIndex vector store (`StorageContext.from_defaults(vector_store=vector_store)`).

A complete retrieval-augmented example without external services is in [`sdk/python/examples/rag_langchain.py`](https://github.com/DikshitRJ/arkivector/blob/main/sdk/python/examples/rag_langchain.py).
