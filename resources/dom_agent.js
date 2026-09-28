// This closure runs in the selected document. Transport, deadlines, polling and
// trusted input remain in C++; no persistent names are installed in the page.
(() => {
  const tidy = value => String(value ?? '').replace(/\s+/g, ' ').trim();
  const visible = node => {
    if (!node?.isConnected) return false;
    const style = getComputedStyle(node), rect = node.getBoundingClientRect();
    return style.visibility !== 'hidden' && style.visibility !== 'collapse' &&
      style.display !== 'none' && rect.width > 0 && rect.height > 0;
  };
  const enabled = node => !!node && !node.matches(':disabled') &&
    !node.closest('[aria-disabled="true"], [inert]');
  const editable = node => enabled(node) && !node.readOnly &&
    (node.isContentEditable || node.matches('textarea, input:not([type=hidden]):not([type=file]):not([type=checkbox]):not([type=radio]):not([type=button]):not([type=submit]):not([type=reset]):not([type=image])'));
  const roots = () => {
    const result = [document];
    for (let i = 0; i < result.length; ++i) {
      for (const node of result[i].querySelectorAll('*')) {
        if (node.shadowRoot) result.push(node.shadowRoot);
      }
      if (result.length > 10000) throw Error('Shadow root limit exceeded');
    }
    return result;
  };
  const all = query => roots().flatMap(root => [...root.querySelectorAll(query)]);
  const textCache = new WeakMap();
  let textVisits = 0;
  const contentOf = (node, depth = 0) => {
    if (textCache.has(node)) return textCache.get(node);
    if (++textVisits > 100000 || depth > 256) throw Error('Text traversal limit exceeded');
    if (node.nodeType === Node.TEXT_NODE) return node.nodeValue || '';
    if (node.nodeType !== Node.ELEMENT_NODE && node.nodeType !== Node.DOCUMENT_FRAGMENT_NODE) return '';
    if (node.nodeType === Node.ELEMENT_NODE && node.matches('script,style,head,noscript,template')) return '';
    let text = '';
    if (node.nodeType === Node.ELEMENT_NODE && node.matches('input[type=button],input[type=submit]')) text = node.value;
    else {
      for (const child of node.childNodes) text += contentOf(child, depth + 1);
      if (node.shadowRoot) text += contentOf(node.shadowRoot, depth + 1);
    }
    textCache.set(node, text);
    return text;
  };
  const textOf = node => tidy(contentOf(node));
  const labelOf = node => {
    const referenced = node.getAttribute('aria-labelledby');
    if (referenced) return tidy(referenced.split(/\s+/).map(id => node.getRootNode().getElementById?.(id)?.textContent ?? '').join(' '));
    if (node.hasAttribute('aria-label')) return tidy(node.getAttribute('aria-label'));
    if (node.labels?.length) return tidy([...node.labels].map(label => label.textContent).join(' '));
    return '';
  };
  const cssGroups = selector => {
    const groups=[],nesting=[];
    let start=0,suffix=-1,quote='',escaped=false,comment=false;
    const finish=end=>{
      const value=selector.slice(start,end).trim();
      if(!value)throw Error('Empty CSS selector branch');
      const filter=suffix>=start && !selector.slice(suffix+8,end).replace(/\/\*[\s\S]*?\*\//g,'').trim();
      groups.push({query:filter?(selector.slice(start,suffix).trim()||'*'):value,filter});
    };
    for(let i=0;i<selector.length;i++){
      const c=selector[i],next=selector[i+1];
      if(comment){if(c==='*'&&next==='/'){comment=false;i++}continue}
      if(escaped){escaped=false;continue}
      if(c==='\\'){escaped=true;continue}
      if(quote){if(c===quote)quote='';continue}
      if(c==='/'&&next==='*'){comment=true;i++;continue}
      if(c==='"'||c==="'"){quote=c;continue}
      if(c==='('||c==='['){nesting.push(c);continue}
      if(c===')'||c===']'){
        if(nesting.pop()!==(c===')'?'(':'['))throw Error('Unbalanced CSS selector');
        continue;
      }
      if(!nesting.length&&c===','){finish(i);start=i+1;suffix=-1;continue}
      if(!nesting.length&&c===':'&&selector.slice(i,i+8)===':visible')suffix=i;
    }
    if(quote||escaped||comment||nesting.length)throw Error('Unclosed CSS selector');
    finish(selector.length);return groups;
  };
  const select = selector => {
    if (typeof selector !== 'string' || !selector.trim()) throw Error('A nonempty selector is required');
    if (selector.startsWith('css=')) selector = selector.slice(4);
    if (selector.startsWith('xpath=') || selector.startsWith('//') || selector.startsWith('..')) {
      const path = selector.startsWith('xpath=') ? selector.slice(6) : selector;
      const list = document.evaluate(path, document, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
      return Array.from({length:list.snapshotLength}, (_,i) => list.snapshotItem(i)).filter(node => node.nodeType === Node.ELEMENT_NODE);
    }
    if (selector.startsWith('id=')) return all('[id]').filter(node => node.id === selector.slice(3));
    if (selector.startsWith('data-testid=')) return all('[data-testid]').filter(node => node.getAttribute('data-testid') === selector.slice(12));
    if (selector.startsWith('placeholder=')) return all('[placeholder]').filter(node => tidy(node.getAttribute('placeholder')).includes(tidy(selector.slice(12))));
    if (selector.startsWith('label=')) return all('input,textarea,select,button,[aria-label],[aria-labelledby]').filter(node => labelOf(node).includes(tidy(selector.slice(6))));
    if (selector.startsWith('text=')) {
      const wanted = tidy(selector.slice(5));
      const candidates = all('*').filter(node => !node.matches('script,style,head,title') && textOf(node).includes(wanted));
      return candidates.filter(node => ![...node.children].some(child => candidates.includes(child)));
    }
    if (selector.startsWith('role=')) throw Error('Role selectors must be resolved through Chrome accessibility');
    // Support the common Playwright :visible suffix without silently stripping
    // unsupported engines or arbitrary pseudo selectors.
    const groups=cssGroups(selector);
    if(groups.length===1)return all(groups[0].query).filter(node=>!groups[0].filter||visible(node));
    const matches=new Set(groups.flatMap(group=>all(group.query).filter(node=>!group.filter||visible(node))));
    // A selector union follows document order, including each host's light
    // children followed by its shadow children, rather than branch order.
    const ordered=[],pending=[...document.children].reverse();
    while(pending.length){
      const node=pending.pop();
      if(matches.has(node))ordered.push(node);
      if(node.shadowRoot)for(let i=node.shadowRoot.children.length;i-->0;)pending.push(node.shadowRoot.children[i]);
      for(let i=node.children.length;i-->0;)pending.push(node.children[i]);
    }
    return ordered;
  };
  const query = args => {
    let candidates;
    if (args.field !== undefined) {
      const field = args.field;
      if (/^[#.\[]/.test(field)) return select(field);
      candidates = select('label=' + field);
      if (!candidates.length) candidates = all('[name]').filter(node => node.getAttribute('name') === field);
    } else if (args.label !== undefined) candidates = select('label=' + args.label);
    else if (args.placeholder !== undefined) candidates = select('placeholder=' + args.placeholder);
    else if (args.text !== undefined && args.targetText) candidates = select('text=' + args.text);
    else if (args.attribute !== undefined && args.searchAttribute) {
      candidates = all(args.tag || '*').filter(node => node.hasAttribute(args.attribute) &&
        (args.value === undefined || node.getAttribute(args.attribute) === args.value));
    } else if (args.inputIndex) candidates = select('input:visible,textarea:visible');
    else candidates = select(args.selector || '');
    if (args.tag && args.targetText) candidates = all(args.tag).filter(node => textOf(node).includes(tidy(args.text)));
    return candidates;
  };
  const stateOf = (node, state) => {
    const exists = !!node?.isConnected;
    switch (state) {
      case 'attached': case 'exists': return exists;
      case 'detached': return !exists;
      case 'visible': return visible(node);
      case 'hidden': return !visible(node);
      case 'enabled': return exists && enabled(node);
      case 'disabled': return exists && !enabled(node);
      case 'checked': return exists && (node.checked === true || node.getAttribute('aria-checked') === 'true');
      case 'focused': return exists && node.getRootNode().activeElement === node;
      case 'editable': return exists && editable(node);
      case 'in_viewport': {
        if (!visible(node)) return false;
        const box = node.getBoundingClientRect();
        return box.top >= 0 && box.left >= 0 && box.bottom <= innerHeight && box.right <= innerWidth;
      }
      default: throw Error('Unknown element state: ' + state);
    }
  };
  const on = (node, args) => {
    const kind = args.operation;
    if (kind === 'state') return stateOf(node, args.state);
    if (kind === 'click_probe') {
      if (!node?.isConnected) return {attached:false,ready:false};
      if (!visible(node) || !enabled(node)) return {attached:true,ready:false};
    }
    if (!node?.isConnected) throw Error('Element is detached');
    if (kind === 'ready') return visible(node) && (!args.enabled || enabled(node)) && (!args.editable || editable(node));
    if (kind === 'scroll') { node.scrollIntoView({block:'center',inline:'center',behavior:'instant'}); return true; }
    if (kind === 'point' || kind === 'click_probe') {
      const box = node.getBoundingClientRect();
      const x = (Math.max(0,box.left) + Math.min(innerWidth,box.right))/2;
      const y = (Math.max(0,box.top) + Math.min(innerHeight,box.bottom))/2;
      let candidate=node,hit=true;
      while(candidate){
        const root=candidate.getRootNode(),top=root.elementFromPoint(x,y);
        if(!top || !(candidate===top || candidate.contains(top))){hit=false;break}
        candidate=root.host;
      }
      return {x,y,width:box.width,height:box.height,hit,text:textOf(node).slice(0,100),
        ...(kind==='click_probe'?{attached:true,ready:true}:{})};
    }
    if (kind === 'focus') { node.focus(); return node.getRootNode().activeElement === node; }
    if (kind === 'blur') { node.blur(); return true; }
    if (kind === 'file_input') {
      if(!node.matches('input[type=file]'))throw Error('Element is not a file input');
      return {multiple:node.multiple,directory:node.webkitdirectory,count:node.files.length};
    }
    if (kind === 'clear_files') {
      if(!node.matches('input[type=file]'))throw Error('Element is not a file input');
      node.value='';
      node.dispatchEvent(new Event('input',{bubbles:true,composed:true}));
      node.dispatchEvent(new Event('change',{bubbles:true}));
      return {count:node.files.length};
    }
    if (kind === 'highlight') {
      const color=args.color??'red';
      if(color.length>256||!CSS.supports('outline-color',color))throw Error('Invalid highlight color');
      const key=Symbol.for('PagePilot.outlineLease');
      const fields=['outline-width','outline-style','outline-color'];
      const capture=()=>fields.map(name=>[name,node.style.getPropertyValue(name),node.style.getPropertyPriority(name)]);
      const old=node[key];
      if(old)clearTimeout(old.timer);
      // Keep the pre-highlight values across overlapping calls, but do not
      // restore stale values when page code replaced the outline in between.
      const before=capture().map((value,index)=>old&&JSON.stringify(value)===JSON.stringify(old.applied[index])?old.before[index]:value);
      node.style.setProperty('outline-width','3px','important');
      node.style.setProperty('outline-style','solid','important');
      node.style.setProperty('outline-color',color,'important');
      const lease={before,applied:capture(),timer:0};
      node[key]=lease;
      lease.timer=setTimeout(()=>{
        if(node[key]!==lease)return;
        const current=capture();
        lease.before.forEach(([name,value,priority],index)=>{
          if(JSON.stringify(current[index])!==JSON.stringify(lease.applied[index]))return;
          if(value)node.style.setProperty(name,value,priority);else node.style.removeProperty(name);
        });
        delete node[key];
      },args.duration??2000);
      return true;
    }
    if (kind === 'prepare_input') {
      if (!editable(node)) throw Error('Element is not editable');
      node.focus();
      if (node.isContentEditable) {
        const range = document.createRange(); range.selectNodeContents(node);
        if (!args.clear) range.collapse(false);
        const selection = node.getRootNode().getSelection?.() ?? getSelection();
        selection.removeAllRanges(); selection.addRange(range);
      } else {
        const type = node.type;
        if (node.localName === 'input' && ['date','datetime-local','time','month','week','color','range'].includes(type)) {
          const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set;
          setter.call(node,args.text);
          if (node.value !== args.text) throw Error('Input rejected the supplied value');
          node.dispatchEvent(new Event('input',{bubbles:true,composed:true}));
          node.dispatchEvent(new Event('change',{bubbles:true}));
          return {assigned:true};
        }
        if (args.clear) node.select();
        else { try { node.setSelectionRange(node.value.length,node.value.length); } catch { throw Error('Append is unsupported for this input type'); } }
      }
      return {assigned:false};
    }
    if (kind === 'select') {
      if (node.localName !== 'select') throw Error('Element is not a select control');
      if (!enabled(node)) throw Error('Select control is disabled');
      const choices = [...node.options];
      const chosen = args.value !== undefined ? choices.find(option => option.value === args.value) :
        args.index !== undefined ? choices[args.index] : choices.find(option => option.label === args.text);
      if (!chosen || chosen.disabled || chosen.parentElement.matches('optgroup:disabled')) throw Error('Requested option is missing or disabled');
      for (const option of choices) option.selected = option === chosen;
      node.dispatchEvent(new Event('input',{bubbles:true,composed:true}));
      node.dispatchEvent(new Event('change',{bubbles:true}));
      return {selected:args.value ?? args.index ?? args.text,value:node.value};
    }
    if (kind === 'classes') return {has_class:node.classList.contains(args.class_name),class_name:args.class_name,all_classes:node.getAttribute('class') || ''};
    if (kind === 'summary') return {tag:node.localName,text:node.textContent?.slice(0,100) || '',id:node.id || '',class:node.getAttribute('class') || ''};
    if (kind === 'checkbox_kind') {
      if (!node.matches('input[type=checkbox],input[type=radio],[role=checkbox],[role=radio],[role=switch]')) throw Error('Element is not a checkbox or radio control');
      return true;
    }
    if (kind === 'scroll_by') {node.scrollBy(args.x,args.y);return {x:node.scrollLeft,y:node.scrollTop};}
    if (kind !== 'read') throw Error('Unknown DOM operation: ' + kind);
    const type = args.type || 'info';
    if (type === 'text') return {text:node.textContent || args.fallback || ''};
    if (type === 'html') return {html:args.outer ? node.outerHTML : node.innerHTML};
    if (type === 'attribute') {
      if (!args.attribute) throw Error('Attribute name is required');
      return {[args.attribute]:node.getAttribute(args.attribute)};
    }
    if (type === 'value') return {value:node.isContentEditable ? node.textContent : node.value};
    if (['position','dimensions','bounding_box'].includes(type)) {
      if (!visible(node)) return {error:'Unable to get bounds'};
      const {x,y,width,height} = node.getBoundingClientRect();return {x,y,width,height};
    }
    if (type === 'styles') {
      const computed = getComputedStyle(node), result = {};
      if (args.properties?.length) for (const name of args.properties) result[name] = computed.getPropertyValue(name);
      else Object.assign(result,{display:computed.display,color:computed.color,backgroundColor:computed.backgroundColor});
      return {styles:result};
    }
    if (type === 'classes') return {classes:[...node.classList]};
    if (type === 'tag') return {tag:node.localName};
    if (type === 'dataset') return {dataset:{...node.dataset}};
    return {tag:node.localName,id:node.id,classes:node.getAttribute('class') || '',text:node.textContent?.slice(0,200),value:node.value,href:node.href,src:node.src};
  };
  const run = args => {
    const candidates = query(args), chosen = candidates[args.index || 0] ?? null;
    if (args.operation === 'locate') return chosen;
    if (args.operation === 'count') return {count:candidates.length};
    if (args.operation === 'state') return {result:stateOf(chosen,args.state),count:candidates.length};
    if (args.operation === 'find') {
      const elements = candidates.slice(0,args.limit ?? 10).map((node,index) => ({index,tag:node.localName,text:node.textContent?.slice(0,100) || '',id:node.id || '',class:node.getAttribute('class') || ''}));
      return {found:elements.length,total:candidates.length,elements};
    }
    return on(chosen,args);
  };
  return {run,on};
})()
