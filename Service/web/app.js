const $=selector=>document.querySelector(selector);
const form=$('#search-form'),queryInput=$('#query'),modeInput=$('#mode'),status=$('#status'),workspace=$('#workspace'),results=$('#results'),pagination=$('#pagination'),previous=$('#previous'),next=$('#next'),pageNumber=$('#page-number'),preview=$('#preview'),resizer=$('#preview-resizer'),previewTitle=$('#preview-title'),documentContent=$('#document-content'),documentFrame=$('#document-frame'),openLink=$('#open-link');
let query='',mode='text',offset=0,nextOffset=null,loading=false;

const titleFromPath=path=>{if(!path)return'Untitled document';const clean=path.replace(/[\\/]+$/,'');return clean.slice(Math.max(clean.lastIndexOf('/'),clean.lastIndexOf('\\'))+1)||path};
const message=(text,error=false)=>{status.textContent=text;status.className=error?'error':''};

// Add another MIME type here to support a new document renderer.
const renderers=new Map([
  ['text/markdown',content=>{
    if(!globalThis.marked||!globalThis.DOMPurify)return false;
    const math=[];
    content=content.replace(/\$\$[\s\S]*?\$\$|(?<!\\)\$(?!\$)[^$\n]+?\$|\\\[[\s\S]*?\\\]|\\\([\s\S]*?\\\)/g,value=>`MOONSHOTMATH${math.push(value)-1}TOKEN`);
    documentContent.className='markdown';
    documentContent.innerHTML=DOMPurify.sanitize(marked.parse(content));
    const walker=document.createTreeWalker(documentContent,NodeFilter.SHOW_TEXT);
    while(walker.nextNode())walker.currentNode.nodeValue=walker.currentNode.nodeValue.replace(/MOONSHOTMATH(\d+)TOKEN/g,(_,index)=>math[index]);
    if(globalThis.renderMathInElement)renderMathInElement(documentContent,{delimiters:[{left:'$$',right:'$$',display:true},{left:'\\[',right:'\\]',display:true},{left:'\\(',right:'\\)',display:false},{left:'$',right:'$',display:false}],throwOnError:false,strict:false});
    const diagrams=[...documentContent.querySelectorAll('pre > code.language-mermaid')];
    if(globalThis.mermaid&&diagrams.length){mermaid.initialize({startOnLoad:false,securityLevel:'strict'});const nodes=diagrams.map(code=>code.parentElement);nodes.forEach((node,index)=>{node.className='mermaid';node.textContent=diagrams[index].textContent});mermaid.run({nodes,suppressErrors:true})}
    documentContent.querySelectorAll('a').forEach(link=>{link.target='_blank';link.rel='noopener'});
    return true;
  }],
]);

function renderDocument(body){
  documentContent.className='';
  documentContent.replaceChildren();
  const type=(body.content_type||'text/plain').split(';',1)[0];
  if(!renderers.get(type)?.(body.content))documentContent.textContent=body.content;
  if(body.truncated)documentContent.append(document.createTextNode('\n\n[Preview truncated]'));
}

async function search(){
  if(loading)return;
  loading=true;message('Searching…');pagination.hidden=true;
  try{
    const response=await fetch('/v1/search',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode,query,offset,limit:20})});
    const body=await response.json();
    if(!response.ok)throw new Error(body.error?.message||`HTTP ${response.status}`);
    results.replaceChildren();
    for(const hit of body.results){
      const button=document.createElement('button'),name=document.createElement('span'),path=document.createElement('span'),score=document.createElement('span');
      button.className='result';button.type='button';name.className='result-title';path.className='result-path';score.className='result-score';
      name.textContent=titleFromPath(hit.path);path.textContent=hit.path||`Document ${hit.document_id}`;score.textContent=`Score ${hit.score==null?'—':Number(hit.score).toFixed(2)}`;
      button.append(name,path,score);button.addEventListener('click',()=>showDocument(hit));results.append(button);
    }
    nextOffset=body.next_offset;previous.disabled=offset===0;next.disabled=!body.has_more;pageNumber.textContent=`Page ${Math.floor(offset/20)+1}`;pagination.hidden=!body.returned&&offset===0;
    message(`${body.returned} result${body.returned===1?'':'s'} · ${Number(body.took_ms).toFixed(2)} ms`);
    if(!body.returned)results.innerHTML='<p class="message">No results found.</p>';
  }catch(error){message(error.message,true)}finally{loading=false}
}

async function showDocument(hit){
  message('Loading document…');
  try{
    const response=await fetch(`/v1/documents/${encodeURIComponent(hit.document_id)}`),body=await response.json();
    if(!response.ok)throw new Error(body.error?.message||`HTTP ${response.status}`);
    preview.hidden=false;workspace.classList.add('with-preview');previewTitle.textContent=titleFromPath(hit.path);
    if(body.kind==='url'){
      documentContent.hidden=true;documentFrame.hidden=false;documentFrame.src=body.url;openLink.href=body.url;
    }else{
      documentFrame.hidden=true;documentFrame.removeAttribute('src');documentContent.hidden=false;renderDocument(body);openLink.href=`/v1/documents/${encodeURIComponent(hit.document_id)}?raw=1`;
    }
    openLink.hidden=false;message('');
  }catch(error){message(error.message,true)}
}

form.addEventListener('submit',event=>{event.preventDefault();if(loading)return;query=queryInput.value.trim();mode=modeInput.value;if(query){offset=0;search()}});
previous.addEventListener('click',()=>{offset=Math.max(0,offset-20);search()});
next.addEventListener('click',()=>{if(nextOffset!==null){offset=nextOffset;search()}});
$('#close-preview').addEventListener('click',()=>{preview.hidden=true;workspace.classList.remove('with-preview');documentFrame.removeAttribute('src')});

const resizePreview=width=>workspace.style.setProperty('--preview-width',`${Math.max(320,Math.min(width,workspace.clientWidth*.8))}px`);
let resizePointer=null;
resizer.addEventListener('pointerdown',event=>{resizePointer=event.pointerId;document.body.classList.add('resizing');event.preventDefault()});
window.addEventListener('pointermove',event=>{if(event.pointerId===resizePointer)resizePreview(workspace.getBoundingClientRect().right-event.clientX)});
const stopResize=event=>{if(event.pointerId===resizePointer){resizePointer=null;document.body.classList.remove('resizing')}};
window.addEventListener('pointerup',stopResize);
window.addEventListener('pointercancel',stopResize);
resizer.addEventListener('keydown',event=>{if(event.key==='ArrowLeft'||event.key==='ArrowRight'){event.preventDefault();resizePreview(preview.offsetWidth+(event.key==='ArrowLeft'?20:-20))}});
