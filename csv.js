// SPDX-License-Identifier: GPL-3.0-only
// Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
// The engine emits unquoted numeric fields and GS/T identifiers.
function parseRoutesCsv(text) {
  const lines = text.replace(/^\uFEFF/, "").trim().split(/\r?\n/);
  if (lines.shift() !== "time,route,source,station,firstSat,exitSat,satHops,uplinkMs,satToStationMs,totalMs,path") throw new Error("Unsupported CSV header");
  let previous = -1;
  return lines.filter(line => line.trim()).map((line,index)=>{const row=parseRouteRow(line,index+2,previous);previous=row[0];return row;});
}
function parseRouteRow(line,lineNumber,previous=-1) {
    const r = line.split(",");
    if (r.length !== 11 || !/^(GS|T)\d+$/.test(r[2])) throw new Error(`Invalid row ${lineNumber}`);
    const numeric = [0,1,3,4,5,6,7,8,9];
    if (numeric.some(i => !r[i].trim() || !Number.isFinite(Number(r[i])) || Number(r[i]) < 0)) throw new Error(`Invalid number at row ${lineNumber}`);
    if ([1,3,4,5,6].some(i => !Number.isInteger(Number(r[i])))) throw new Error("Invalid integer or path");
    if (!/^\d+(?:-\d+)*$/.test(r[10])) throw new Error("Invalid integer or path");
    const path = r[10].split("-").map(Number), t = Number(r[0]);
    if (t < previous || path[0] !== Number(r[4]) || path[path.length-1] !== Number(r[5]) || path.length-1 !== Number(r[6])) throw new Error("Unsorted time or inconsistent path");
    return [t, Number(r[1]), Number(r[2].replace(/^[A-Z]+/, "")), Number(r[3]), Number(r[6]), Number(r[9]), path, r[2].startsWith("T") ? 1 : 0, Number(r[4]), Number(r[5]), Number(r[7]), Number(r[8])];
}

// Store every row; only materialize arrays for the visible route window.
class PackedRoutes {
  constructor(){this.blocks=[];this.length=0;this.hops=0;this.delay=0;this.sortedIds=true;this.lastId=-1;}
  push(row){
    const size=8192,index=this.length%size;
    if(index===0){this.finish();this.blocks.push({values:new Float64Array(size*11),offsets:new Uint32Array(size+1),path:[]});}
    const block=this.blocks.at(-1),at=index*11;this.hops+=row[4];this.delay+=row[5];
    block.values.set([row[0],row[1],row[2],row[3],row[4],row[5],row[7],row[10],row[11],this.hops,this.delay],at);
    block.path.push(...row[6]);block.offsets[index+1]=block.path.length;
    this.sortedIds&&=row[1]>=this.lastId;this.lastId=row[1];this.length++;
  }
  finish(){const block=this.blocks.at(-1);if(block&&Array.isArray(block.path))block.path=Float64Array.from(block.path);}
  value(index,column){return this.blocks[Math.floor(index/8192)].values[(index%8192)*11+column];}
  row(index){if(index<0||index>=this.length)return undefined;const b=this.blocks[Math.floor(index/8192)],i=index%8192,v=b.values,a=i*11,path=Array.from(b.path.subarray(b.offsets[i],b.offsets[i+1]));return[v[a],v[a+1],v[a+2],v[a+3],v[a+4],v[a+5],path,v[a+6],path[0],path.at(-1),v[a+7],v[a+8]];}
  at(index){return this.row(index<0?this.length+index:index);}
  upperBound(time){let lo=0,hi=this.length;while(lo<hi){const mid=Math.floor((lo+hi)/2);if(this.value(mid,0)<=time)lo=mid+1;else hi=mid;}return lo;}
  sumsAt(end){return end?[this.value(end-1,9),this.value(end-1,10)]:[0,0];}
  slice(start=0,end=this.length){if(start<0)start=Math.max(0,this.length+start);if(end<0)end=Math.max(0,this.length+end);const out=[];for(let i=start;i<Math.min(end,this.length);i++)out.push(this.row(i));return out;}
  findById(id){if(this.sortedIds){let lo=0,hi=this.length;while(lo<hi){const m=Math.floor((lo+hi)/2);if(this.value(m,1)<id)lo=m+1;else hi=m;}return lo<this.length&&this.value(lo,1)===id?this.row(lo):undefined;}for(let i=0;i<this.length;i++)if(this.value(i,1)===id)return this.row(i);}
  recent(time,src,dst,limit,lifetime){const out=[];for(let i=this.upperBound(time)-1;i>=0&&out.length<limit;i--){if(time-this.value(i,0)>lifetime)break;if((src==='-1'||src===(this.value(i,6)?'T:':'GS:')+this.value(i,2))&&(dst===-1||dst===this.value(i,3)))out.push(this.row(i));}return out;}
  *[Symbol.iterator](){for(let i=0;i<this.length;i++)yield this.row(i);}
}
async function parseRoutesBlob(blob,validate=()=>{},progress=()=>{}) {
  if(blob.size<=4*1024*1024){const rows=parseRoutesCsv(await blob.text());for(const row of rows)validate(row);return rows;}
  const rows=new PackedRoutes(),decoder=new TextDecoder('utf-8',{fatal:true}),chunk=1024*1024;
  let pending='',lineNumber=0,previous=-1;
  const consume=line=>{lineNumber++;if(lineNumber===1){if(line.replace(/^\uFEFF/,'').replace(/\r$/,'')!=='time,route,source,station,firstSat,exitSat,satHops,uplinkMs,satToStationMs,totalMs,path')throw Error('Unsupported CSV header');return;}if(!line.trim())return;const row=parseRouteRow(line.replace(/\r$/,''),lineNumber,previous);validate(row);previous=row[0];rows.push(row);};
  for(let offset=0;offset<blob.size;offset+=chunk){const bytes=await blob.slice(offset,offset+chunk).arrayBuffer();const text=pending+decoder.decode(bytes,{stream:true}),last=text.lastIndexOf('\n');if(last>=0){for(const line of text.slice(0,last).split('\n'))consume(line);pending=text.slice(last+1);}else pending=text;progress(Math.min(1,(offset+bytes.byteLength)/blob.size));await new Promise(resolve=>setTimeout(resolve,0));}
  pending+=decoder.decode();if(pending)consume(pending);rows.finish();
  return new Proxy(rows,{get(target,key,receiver){return typeof key==='string'&&/^\d+$/.test(key)?target.row(Number(key)):Reflect.get(target,key,receiver);}});
}
function findRouteById(data,id){return data.findById?data.findById(id):data.find(row=>row[1]===id);}
function sameRoutes(a,b){if(a===b)return true;if(a.blocks&&b.blocks){if(a.length!==b.length)return false;return a.blocks.every((block,i)=>block.values.every((v,j)=>v===b.blocks[i].values[j])&&block.offsets.every((v,j)=>v===b.blocks[i].offsets[j])&&block.path.length===b.blocks[i].path.length&&block.path.every((v,j)=>v===b.blocks[i].path[j]));}if(a.length!==b.length)return false;for(let i=0;i<a.length;i++){const x=a[i],y=b[i];for(let k=0;k<x.length;k++)if(k===6?x[k].length!==y[k].length||x[k].some((v,j)=>v!==y[k][j]):x[k]!==y[k])return false;}return true;}
if(typeof module!=="undefined")module.exports={parseRoutesCsv,parseRoutesBlob,findRouteById,sameRoutes,PackedRoutes};
