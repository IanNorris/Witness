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
