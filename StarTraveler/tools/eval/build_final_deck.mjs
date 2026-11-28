// Local Artifact Tool fallback: the live session service requires unavailable MXC.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
const runtime='C:/Program Files/WindowsApps/OpenAI.CodexPrimaryRuntime.v26-1123-915-0_26.1123.915.0_x64__3k8sg7r9htsxt/dependencies';
process.env.RUNTIME_NODE_MODULES=path.join(runtime,'node/node_modules');
process.env.RUNTIME_NODE=path.join(runtime,'node/bin/node.exe');
const skill='C:/Users/18612/.codex/plugins/cache/openai-primary-runtime/presentations/26.1123.11515/skills/presentations';
const {Presentation,PresentationFile}=await import(pathToFileURL(path.join(runtime,'node/node_modules/@oai/artifact-tool/dist/artifact_tool.mjs')));
const {finalizePresentation}=await import(pathToFileURL(path.join(skill,'container_tools/artifact_tool_utils.mjs')));
const out=path.join(root,'docs/final-materials');
const revision=process.argv[2] || 'r1';
if(!['r1','r2'].includes(revision)) throw new Error('Expected r1 or r2');
const build=path.join(root,'.final-materials-build',revision);
await fs.mkdir(build,{recursive:true});
await fs.mkdir(path.join(build,'slides'),{recursive:true});
const content=JSON.parse(await fs.readFile(path.join(out,revision==='r2'?'slides-r2.json':'slides.json'),'utf8'));
let qualityRows=[];
if(revision==='r2'){
  const q=(await fs.readFile(path.join(root,'renders/quality-fixed-exposure/summary.csv'),'utf8')).trim().split(/\r?\n/);
  const headers=q.shift().split(',');
  qualityRows=q.map(l=>Object.fromEntries(l.split(',').map((v,i)=>[headers[i],v])));
}
const lines=(await fs.readFile(path.join(root,'renders/production-lag-fixed-exposure/summary.csv'),'utf8')).trim().split(/\r?\n/);
const cols=lines.shift().split(',');
const rows=lines.map(l=>Object.fromEntries(l.split(',').map((v,i)=>[cols[i],v])));
const deck=Presentation.create({slideSize:{width:1280,height:720}});
const ink='#12283D', muted='#4F6272', blue='#1869A6', orange='#C65A20';
function text(slide,str,x,y,w,h,size=28,color=ink,bold=false){
  const t=slide.shapes.add({geometry:'textbox',position:{left:x,top:y,width:w,height:h},fill:'none',line:{fill:'none',width:0}});
  t.text=str; t.text.style={fontFamily:'Arial',fontSize:size,color,bold}; return t;
}
for(const [i,c] of content.entries()){
  const s=deck.slides.add(); s.background.fill=i===0?'#101B2A':'#FFFFFF';
  if(i===0){text(s,c.title,72,160,1120,150,64,'#FFFFFF',true);text(s,c.subtitle,76,365,1100,200,30,'#C6D6E5');}
  else {
    text(s,c.title,64,46,1152,110,42,ink,true);
    if(c.body) c.body.forEach((p,j)=>text(s,p,72,190+j*100,1120,86,29,ink));
    if(c.chart){
      const terminal=c.chart==='terminal';
      const metric=terminal?'terminal_flip_median':'settle_median';
      s.charts.add('bar',{
        position:{left:80,top:168,width:1120,height:410},
        categories:['Anvil cockpit','Mirror / glass','Star turn'],
        series:['asvgf-dlss-full','rr-quality'].map((m,j)=>({name:j?'Hybrid + DLSS-RR':'Full + DLSS K',fill:j?orange:blue,
          values:['cockpit-anvil-turn','mirror-glass-pass','fast-star-turn'].map(shot=>Number(Number(rows.find(r=>r.shot===shot&&r.config===m&&r.reference==='primary')[metric]).toFixed(8)))})),
        barOptions:{direction:'column',grouping:'clustered',gapWidth:110},
        hasLegend:true, legend:{position:'bottom',textStyle:{typeface:'Arial',fontSize:22,fill:ink}},
        xAxis:{textStyle:{typeface:'Arial',fontSize:23,fill:ink}},
        yAxis:{min:0,max:terminal?.05:320,majorUnit:terminal?.01:80,numberFormatCode:terminal?'0.00':'0',
          title:{text:terminal?'Mean LDR-FLIP':'Still-frame index',textStyle:{typeface:'Arial',fontSize:21,fill:ink}},
          textStyle:{typeface:'Arial',fontSize:20,fill:muted},majorGridlines:{fill:'#DDE4E9',width:1}},
        dataLabels:{showValue:true,position:'outEnd',numberFormatCode:terminal?'0.00000':'0',textStyle:{typeface:'Arial',fontSize:22,fill:ink}},
      });
    }
    if(c.image) s.images.add({blob:new Uint8Array(await fs.readFile(path.join(root,'renders/production-lag-fixed-exposure/final-frame-contact-sheet.png'))),contentType:'image/png',alt:'Seed 29 final stationary frames: Full DLSS K, RR, and reference',position:{left:305,top:155,width:670,height:490},fit:'contain'});
    if(c.qualityMetric){
      const cost=c.qualityMetric==='gpu_ms_median';
      s.charts.add('bar',{
        position:{left:80,top:168,width:1120,height:410},
        categories:['Anvil cockpit','Mirror / glass','Star turn'],
        series:['asvgf-dlss-full','asvgf-dlss-half','rr-quality'].map((method,j)=>({
          name:['Full + DLSS K','Half + DLSS K','Hybrid + DLSS-RR'][j],fill:[blue,'#397A54',orange][j],
          values:['cockpit-anvil-turn','mirror-glass-pass','fast-star-turn'].map(shot=>
            Number(Number(qualityRows.find(r=>r.shot===shot&&r.config===method)[c.qualityMetric]).toFixed(8)))})),
        barOptions:{direction:'column',grouping:'clustered',gapWidth:60},hasLegend:true,
        legend:{position:'bottom',textStyle:{typeface:'Arial',fontSize:22,fill:ink}},
        xAxis:{textStyle:{typeface:'Arial',fontSize:23,fill:ink}},
        yAxis:{min:0,max:cost?Math.ceil(Math.max(...qualityRows.map(r=>Number(r.gpu_ms_median)))*1.2/3)*3:Math.ceil(Math.max(...qualityRows.map(r=>Number(r.flip_median)))*1.15/.025)*.025,majorUnit:cost?3:.025,numberFormatCode:cost?'0':'0.000',
          title:{text:cost?'GPU ms/frame':'Moving endpoint LDR-FLIP',textStyle:{typeface:'Arial',fontSize:21,fill:ink}},
          textStyle:{typeface:'Arial',fontSize:20,fill:muted},majorGridlines:{fill:'#DDE4E9',width:1}},
        dataLabels:{showValue:true,position:'outEnd',numberFormatCode:cost?'0.00':'0.00000',
          textStyle:{typeface:'Arial',fontSize:22,fill:ink}}
      });
    }
    if(c.caption)text(s,c.caption,72,c.image?648:605,1120,49,20,muted);
    text(s,`${i+1} / ${content.length}`,1120,684,100,26,17,muted);
  }
  s.speakerNotes.text=`${c.script}\n\n中文提示：${c.cue}\n\nSources: docs/final-denoiser-report.md; docs/production-lag-results.md; renders/production-lag-fixed-exposure/summary.csv and runs.csv; renders/quality-fixed-exposure/summary.csv (moving-path error/time, separate experiment); docs/references-denoiser.md; docs/ai-assistance.md. AI assistance: Codex helped with implementation, analysis automation and this presentation draft. Planned duration ${c.seconds} seconds.`;
}
await (await PresentationFile.exportPptx(deck)).save(path.join(build,'candidate.pptx'));
await fs.writeFile(path.join(build,'content.ndjson'),(await deck.inspect({kind:'slide,textbox,chart,notes',maxChars:100000})).ndjson??'');
for(const [i,s] of deck.slides.items.entries()){
  const png=await deck.export({slide:s,format:'png',scale:1});
  await fs.writeFile(path.join(build,'slides',`slide-${String(i+1).padStart(2,'0')}.png`),new Uint8Array(await png.arrayBuffer()));
  console.log(`Rendered ${i+1}/${content.length}`);
}
const finalPath=path.join(out,revision==='r2'?'StarTraveler-denoiser-presentation-r2.pptx':'StarTraveler-denoiser-presentation.pptx');
const result=await finalizePresentation({workspaceDir:root,candidatePath:path.join(build,'candidate.pptx'),finalPath,
  pythonExecutable:path.join(runtime,'python/python.exe'),
  integrityValidatorPath:path.join(skill,'container_tools/inspect_presentation_package_integrity.py'),
  layoutValidatorPath:path.join(skill,'container_tools/inspect_presentation_layout_geometry.py'),
  layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-bullet-geometry','--validate-heading-fit'],
  explicitTotalSlideCount:content.length,requiredNativeChartOwnerSlides:content.flatMap((c,i)=>c.chart||c.qualityMetric?[i+1]:[]),
  materializeLiteralChartWorkbooks:true,fontPolicy:{basis:'design',families:['Arial']},
  verifyArtifactToolImport:true,receiptPath:path.join(build,'presentation-final.validation.json')});
console.log(JSON.stringify(result));
let elapsed=0;
const speech=['# Twelve-minute speaker script / 十二分钟讲稿','',
  'English slides and script, with Chinese rehearsal cues. Planned total: 12:00. The actual duration needs a spoken rehearsal.',
  '', 'Open `StarTraveler-denoiser-presentation.pptx`. Figures and numbers come from the fixed-exposure production dataset. The older flight video is historical illustration.', ''];
for(const [i,c] of content.entries()){
  const fmt=n=>`${Math.floor(n/60)}:${String(n%60).padStart(2,'0')}`;
  speech.push(c.seconds?`## ${i+1}. ${c.title} (${fmt(elapsed)}–${fmt(elapsed+c.seconds)})`:`## Appendix ${i+1}: ${c.title} (untimed Q&A)`,'',c.script,'',`中文提示：${c.cue}`,'');elapsed+=c.seconds;
}
speech.push('## Questions to prepare / 答辩提示','','- **Why three seeds?** This is a descriptive project protocol, not a universal statistical requirement. We show ranges and make no significance claim.',
  '- **Does RR win?** Earlier entry into its own band on these paths. Full + K has lower observed terminal FLIP on Anvil and star. Mirror values are close.',
  '- **Is mirror one-frame latency?** No. It includes 80 prior motion updates of history.',
  '- **How fast is the new configuration?** Not established by these dump captures. Do not reuse historical Quality timing as a new fixed-input result.',
  '- **Did all GPU tests pass?** No such claim. Three focused regressions pass. The full suite was not rerun, and the deferred M issue remains.',
  '- **Why two references?** To expose reference sensitivity. They do not double the independent render-seed sample size.',
  '- **If the live demo fails?** Show these saved charts and images. Label the old flight clip historical. Step 6 still updates the live rehearsal and backup under the final configuration.',
  '', 'AI assistance disclosure and artifact links are in [the report](../final-denoiser-report.md).');
let scriptText=speech.join('\n')+'\n';
if(revision==='r2') scriptText=scriptText
  .replaceAll('StarTraveler-denoiser-presentation.pptx','StarTraveler-denoiser-presentation-r2.pptx')
  .replace('Figures and numbers come from the fixed-exposure production dataset. The older flight video is historical illustration.','Main figures use the fixed-exposure settling dataset; appendices use the separate 27-run Quality motion study. Use the accepted fixed-exposure flight backup as illustration.')
  .replace('Planned total: 12:00.','Main 12 slides planned: 12:00, plus two untimed Q&A appendices.')
  .replace('Not established by these dump captures. Do not reuse historical Quality timing as a new fixed-input result.','See the separate fixed-input 27-run moving-path study and appendix. Settling dump captures themselves are not timing evidence.')
  .replace('Label the old flight clip historical. Step 6 still updates the live rehearsal and backup under the final configuration.','Use the accepted fixed-exposure flight backup. Manual operation, projection and spoken timing still need a person on the presentation setup.');
await fs.writeFile(path.join(out,revision==='r2'?'speaker-script-r2.md':'speaker-script.md'),scriptText,'utf8');
console.log(`Saved ${finalPath}; planned speech duration ${elapsed}s`);
