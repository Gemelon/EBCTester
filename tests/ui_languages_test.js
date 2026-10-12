"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.resolve(__dirname, "..");
const suite = process.env.OTA_SUITE_DIR || path.join(root, "external", "ota-suite");
const sdManager = process.env.SD_CARD_MANAGER_DIR || path.join(root, "external", "sd-card-manager");
const shared = path.join(suite, "components", "web_interface", "assets");
const en = JSON.parse(fs.readFileSync(path.join(shared,"en.json"),"utf8"));
const de = JSON.parse(fs.readFileSync(path.join(shared,"de.json"),"utf8"));
const source = fs.readFileSync(path.join(shared,"i18n.js"),"utf8");
const id = text => Object.keys(en.strings).find(key => en.strings[key]===text);
assert(id("Battery"));

async function runtime({saved="en",files=[],errors=[],file,offline=false,sdEnabled=true,browser="en-GB"}={}) {
  const warning = {textContent:"",classList:{remove(){}}};
  const staticLabel = {dataset:{i18n:id("Battery")},textContent:""};
  const placeholder = {dataset:{i18nAttrs:`placeholder:${id("Password")}`},setAttribute(name,text){this[name]=text;}};
  const selector = {options:[],replaceChildren(){this.options=[];},append(option){this.options.push(option);},
    addEventListener(name,handler){this.handler=handler;}};
  let stored=saved, reloaded=false;
  const context = {
    navigator:{language:browser},TextEncoder,Intl,AbortSignal,
    localStorage:{getItem(){return stored;},setItem(key,value){stored=value;}},
    location:{reload(){reloaded=true;}},
    document:{
      documentElement:{lang:"en"},
      querySelectorAll(query){return query==="[data-i18n]"?[staticLabel]:[placeholder];},
      getElementById(name){return name==="language-select"?selector:warning;},
      createElement(){return {};},
    },
    console:{warn(){},error(message){throw new Error(message);}},
    fetch:async url=>{
      if(offline) throw new Error("Network offline");
      return {ok:true,json:async()=>url.includes("?code=")?file:{languages:files,errors}};
    },
  };
  context.window=context;
  context.EBCLanguages={en,de,sdEnabled};
  context.confirm=()=>true;
  vm.createContext(context);
  vm.runInContext(source,context);
  return {context,warning,staticLabel,placeholder,selector,
    stored:()=>stored,reloaded:()=>reloaded};
}

async function boot(options) {
  const result = await runtime(options);
  await result.context.I18n.ready;
  return result;
}

(async()=>{
  assert.deepEqual(Object.keys(en.strings).sort(),Object.keys(de.strings).sort());
  const template=JSON.parse(fs.readFileSync(path.join(root,"sd_card","languages","en-custom.json"),"utf8"));
  assert.deepEqual(template.strings,en.strings);
  const filipinoPath=path.join(root,"sd_card","languages","fil.json");
  const filipino=JSON.parse(fs.readFileSync(filipinoPath,"utf8"));
  assert.equal(filipino.version,1);
  assert.equal(filipino.code,"fil");
  assert(fs.statSync(filipinoPath).size<=128*1024);
  assert(Object.keys(filipino.strings).length<=1024);
  for(const key of Object.keys(filipino.strings)) assert(Object.hasOwn(en.strings,key),`Unknown Filipino key: ${key}`);
  const filRuntime=await boot({saved:"fil",files:[{code:filipino.code,name:filipino.name}],file:filipino});
  assert.equal(filRuntime.context.I18n.code,"fil");
  assert.equal(filRuntime.context.I18n.locale,"fil");
  assert.equal(filRuntime.staticLabel.textContent,"Baterya");
  assert.equal(filRuntime.warning.textContent,"");
  assert.equal(Object.keys(filRuntime.context.I18n.validate(filipino,"fil")).length,Object.keys(filipino.strings).length);
  assert.equal(filRuntime.context.I18n.t(id("Chart.js is missing")),"Chart.js is missing");
  assert.equal(filRuntime.context.I18n.f(id("Step {p0} / cycle {p1}"),{p0:2,p1:3}),"Hakbang 2 / siklo 3");
  let r=await boot();
  assert.equal(r.staticLabel.textContent,"Battery");
  assert.equal(r.placeholder.placeholder,"Password");
  assert.equal(r.context.I18n.message("Messung gestartet."),"Measurement started.");
  assert.equal(r.context.I18n.f(id("Step {p0} / cycle {p1}"),{p0:2,p1:3}),"Step 2 / cycle 3");
  r=await boot({saved:"de"});
  assert.equal(r.staticLabel.textContent,"Batterie");
  assert.equal(r.context.I18n.message("Measurement started."),"Messung gestartet.");
  assert.equal(r.context.I18n.message("Step 2 (D-CC) started."),"Schritt 2 (D-CC) gestartet.");
  assert.equal(r.context.I18n.message("Wi-Fi error: ESP_FAIL. Access point remains accessible."),
    "WLAN-Fehler: ESP_FAIL. Access Point bleibt erreichbar.");
  assert.equal(r.context.I18n.message("Test run 1/2 (abc): 50 measurements transferred"),
    "Testlauf 1/2 (abc): 50 Messwerte uebertragen");
  r=await boot({saved:"fr",files:[{code:"fr",name:"Français"}],
    file:{version:1,code:"fr",name:"Français",strings:{[id("Battery")]:"Batterie française"}}});
  assert.equal(r.staticLabel.textContent,"Batterie française");
  assert.equal(r.placeholder.placeholder,"Password");
  assert.equal(r.context.I18n.code,"fr");
  r.selector.value="de";r.selector.handler();
  assert.equal(r.stored(),"de");assert(r.reloaded());
  r=await boot({saved:"fr",files:[{code:"fr",name:"Français"}],
    file:{version:1,code:"fr",name:"Français",strings:{[id("Connection failed:")]:"$&"}}});
  assert.equal(r.context.I18n.message("Connection failed: EPIPE"),"$& EPIPE");
  r=await boot({saved:"fr",files:[{code:"fr",name:"Français"}],
    file:{version:1,code:"fr",name:"Français",strings:{[id("Battery")]:{html:"unsafe"}}}});
  assert.equal(r.context.I18n.code,"en");assert.match(r.warning.textContent,/Invalid language/);
  r=await boot({saved:"fr",files:[{code:"fr",name:"Français"}],
    file:{version:1,code:"fr",name:"Français",strings:{[id("Step {p0} / cycle {p1}")]:"Step {p0}"}}});
  assert.equal(r.context.I18n.code,"en");assert.match(r.warning.textContent,/placeholders/);
  r=await boot({saved:"missing",offline:true});
  assert.equal(r.staticLabel.textContent,"Battery");assert.equal(r.context.I18n.code,"en");
  assert(r.warning.textContent);
  r=await boot({saved:"de",sdEnabled:false});
  assert.equal(r.context.I18n.code,"de");assert.equal(r.warning.textContent,"");
  r=await boot({saved:"fr",files:[{code:"fr",name:"Français"}],
    file:{version:1,code:"fr",name:"Français",strings:{[id("Battery")]:"<img src=x onerror=alert(1)>"}}});
  assert.equal(r.staticLabel.textContent,"<img src=x onerror=alert(1)>");

  let references=0;
  function audit(dir) {
    for(const entry of fs.readdirSync(dir,{withFileTypes:true})) {
      const file=path.join(dir,entry.name);
      if(entry.isDirectory())audit(file);
      else if(/\.(js|html)$/.test(file)&&!/chart\.umd|i18n\.js/.test(file)) {
        const text=fs.readFileSync(file,"utf8");
        for(const match of text.matchAll(/(?:I18n\.[tf]\("|data-i18n=")([^"]+)/g)) {
          assert(Object.hasOwn(en.strings,match[1]),`${file}: missing ${match[1]}`);
          references++;
        }
      }
    }
  }
  audit(path.join(root,"components"));
  audit(shared);
  audit(path.join(sdManager,"components"));
  console.log(`Language tests passed: fallback, overrides, validation, safe text, persistence, standalone UI; ${Object.keys(en.strings).length} keys, ${references} references.`);
})().catch(error=>{console.error(error);process.exitCode=1;});
