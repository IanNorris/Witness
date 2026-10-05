import assert from 'node:assert/strict'
import {readFile} from 'node:fs/promises'
import test from 'node:test'
import ts from 'typescript'
const source=await readFile(new URL('../src/composables/msePresentationProgress.ts',import.meta.url),'utf8')
const output=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.ESNext,target:ts.ScriptTarget.ES2020}}).outputText
const {createPresentationProgress,presentationViewportVisible}=await import('data:text/javascript;base64,'+Buffer.from(output).toString('base64'))
test('presented timestamp advancement resets stalled evidence regardless of static image content',()=>{
  const p=createPresentationProgress();p.presented(0)
  assert.equal(p.poll(250,3,true),250)
  p.presented(1);assert.equal(p.stalledMs,0);assert.equal(p.frames,2)
  p.presented(1);assert.equal(p.poll(250,3,true),250)
  p.presented(2);assert.equal(p.stalledMs,0)
})
test('unknown or ineligible presentation cannot accumulate a stall',()=>{
  const p=createPresentationProgress()
  assert.equal(p.poll(250,3,true),0);p.presented(NaN);assert.equal(p.frames,0)
  p.presented(0);p.poll(250,3,true)
  assert.equal(p.poll(250,3,false),0)
  assert.equal(p.poll(250,.5,true),0)
  assert.equal(p.poll(NaN,3,true),0)
  p.reset();assert.equal(p.mediaTime,undefined);assert.equal(p.frames,0)
})
test('suspension is excluded but repeated delayed runnable ticks still permit recovery',()=>{
  const p=createPresentationProgress();p.presented(0)
  assert.equal(p.poll(60000,3,true),250)
  for(let i=0;i<12;i++)p.poll(25000,3,true)
  assert.equal(p.stalledMs,3250)
})
test('installed callbacks detect missing first presentation only after eligible clock progress',()=>{
  const p=createPresentationProgress();p.observeCallbacks()
  assert.equal(p.poll(250,5,true),0)
  assert.equal(p.poll(250,6,true),0)
  for(let i=0;i<13;i++)p.poll(250,6.25+i*.25,true)
  assert.equal(p.stalledMs,3250);assert.equal(p.frames,0);assert.equal(p.mediaTime,undefined)
  p.presented(9);assert.equal(p.stalledMs,0)
})
test('first presentation grace restarts after ineligibility, a generation reset or clock rewind',()=>{
  const p=createPresentationProgress();p.observeCallbacks()
  p.poll(250,5,true);p.poll(250,7,true);assert.equal(p.stalledMs,250)
  p.poll(250,8,false);assert.equal(p.poll(250,100,true),0)
  assert.equal(p.poll(250,102,true),250)
  p.reset();assert.equal(p.poll(250,200,true),0)
  assert.equal(p.poll(250,202,true),250)
  assert.equal(p.poll(250,0,true),0)
  assert.equal(p.poll(60000,2,true),250,'suspension cannot spend a wall-clock timeout')
})
test('viewport hit testing accepts own controls and excludes external coverage or offscreen rectangles',()=>{
  const control={},outside={}
  const viewport={getBoundingClientRect:()=>({left:0,top:0,right:100,bottom:100}),contains:node=>node===control}
  assert.equal(presentationViewportVisible(viewport,{elementFromPoint:()=>control},200,200),true)
  assert.equal(presentationViewportVisible(viewport,{elementFromPoint:()=>outside},200,200),false)
  assert.equal(presentationViewportVisible(viewport,{elementFromPoint:()=>null},200,200),false)
  assert.equal(presentationViewportVisible(viewport,{elementFromPoint:()=>control},0,200),false)
  viewport.getBoundingClientRect=()=>({left:300,top:0,right:400,bottom:100})
  assert.equal(presentationViewportVisible(viewport,{elementFromPoint:()=>control},200,200),false)
})
