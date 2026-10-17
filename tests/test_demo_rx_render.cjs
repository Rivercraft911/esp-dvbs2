// Static DOM and real CPU canvas rendering. All input is fake; no HTTP or devices.
// npm install --prefix /tmp/rx-dom jsdom @napi-rs/canvas
// NODE_PATH=/tmp/rx-dom/node_modules node tests/test_demo_rx_render.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM} = require('jsdom');
const {createCanvas} = require('@napi-rs/canvas');
const html = fs.readFileSync(path.join(__dirname, '../host/demo_rx.html'), 'utf8');

async function render(webgl) {
    const errors = [], timers = [], calls = [], surfaces = new Map();
    let bitmapClosed = 0, imageReads = 0;
    const state = {source:'B200 / SOAPY', healthy:true, status:'running', symbol_rate:10e6,
        sample_rate:20e6, center_hz:2420e6, received_mbps:14.52, mer_db:15.2,
        mer_age_s:.1, frames_ok:12345, frames_failed:0, mode:'QPSK 3/4',
        input_msps:20, behind_s:.01, source_loops:0, bbheader_failed:0,
        video_frames:12, video_age_s:.1, tx:null, spectrum_seq:1,
        spectrum:Array.from({length:1024},(_,i)=>-95+70*Math.exp(-(((i-512)/200)**8))), error:null};
    // A WebGL API double checks JS calls and controls, not GLSL compilation or GPU pixels.
    const gl = new Proxy({}, {get:(_,key)=> (...args) => {
        calls.push([key,...args]);
        if (key === 'getShaderParameter' || key === 'getProgramParameter') return true;
        if (key.startsWith('create') || key.startsWith('get')) return {};
    }});
    const dom = new JSDOM(html, {url:'http://demo.invalid/', runScripts:'dangerously',
        beforeParse(window) {
            window.addEventListener('error', e=>errors.push(e.message));
            window.ResizeObserver = class {observe() {}};
            window.setTimeout = fn=>timers.push(fn);
            window.URL.createObjectURL = ()=> 'blob:test-frame';
            window.URL.revokeObjectURL = ()=>{};
            window.createImageBitmap = async()=>({width:960,height:480,close(){bitmapClosed++;}});
            window.fetch = async url=>{
                assert.ok(['/api/state','/frame.jpg'].includes(url), url);
                if (url === '/frame.jpg') imageReads++;
                return {ok:true,json:async()=>structuredClone(state),blob:async()=>new window.Blob(['test'])};
            };
            window.HTMLCanvasElement.prototype.getContext = function(kind) {
                if (kind === 'webgl') return webgl ? gl : null;
                if (!surfaces.has(this.id)) surfaces.set(this.id,createCanvas(this.width,this.height));
                const ctx=surfaces.get(this.id).getContext('2d');
                // Canvas-to-canvas copies need the CPU surface rather than the DOM element.
                return new Proxy(ctx,{get:(obj,key)=>key==='drawImage' ? (img,...args)=>
                    obj.drawImage(surfaces.get(img.id),...args) :
                    typeof obj[key]==='function' ? obj[key].bind(obj) : obj[key],
                    set:(obj,key,value)=>{obj[key]=value;return true;}});
            };
            window.HTMLCanvasElement.prototype.getBoundingClientRect = ()=>({width:800,height:450});
            window.HTMLCanvasElement.prototype.setPointerCapture = ()=>{};
            window.HTMLElement.prototype.requestFullscreen = function(){calls.push(['fullscreen',this.id]);};
        }});
    const w=dom.window, doc=w.document, el=id=>doc.getElementById(id);
    const settle=()=>new Promise(resolve=>setImmediate(resolve));
    const loop=async name=>{const fn=timers.find(x=>x.name===name);assert.ok(fn,name);await fn();await settle();};
    await settle();await loop('videoLoop');
    assert.deepEqual(errors,[]);
    assert.equal(el('connection').textContent,'FRAME LOCK');
    assert.equal(el('mode').textContent,'QPSK 3/4');
    assert.equal(el('bitrate').textContent,'14.52');
    assert.equal(el('mer').textContent,'15.2');
    assert.equal(el('center').textContent,'2420.00 MHz');
    assert.equal(el('error').textContent,'');
    assert.equal(el('empty').hidden,true);
    assert.equal(imageReads,1);
    // Ensure every script ID resolves and every declared CSS rule parses.
    for (const [,id] of html.matchAll(/\$\('([^']+)'\)/g)) assert.ok(el(id),id);
    assert.ok(doc.styleSheets[0].cssRules.length>40);
    // Real 2D drawing must produce both a trace and a colored waterfall row.
    const pixels=surfaces.get('spectrum').getContext('2d').getImageData(0,0,1024,220).data;
    let mint=0;for(let n=0;n<pixels.length;n+=4) if(pixels[n+1]>180 && pixels[n+2]>140) mint++;
    assert.ok(mint>100,'missing spectrum trace');
    const row=surfaces.get('waterfall').getContext('2d').getImageData(0,0,1024,1).data;
    assert.notDeepEqual([...row.slice(0,4)],[...row.slice(512*4,512*4+4)]);
    if (webgl) {
        assert.ok(calls.some(x=>x[0]==='texImage2D'),'missing video texture');
        assert.ok(calls.some(x=>x[0]==='drawArrays'),'missing video draw');
        el('pano').click();assert.equal(el('pano').getAttribute('aria-pressed'),'true');
        const before=w.eval('[yaw,pitch,fov]');
        el('view').dispatchEvent(new w.KeyboardEvent('keydown',{key:'ArrowRight',cancelable:true}));
        el('view').dispatchEvent(new w.WheelEvent('wheel',{deltaY:20,cancelable:true}));
        assert.notDeepEqual(w.eval('[yaw,pitch,fov]'),before);
        el('view').dispatchEvent(new w.MouseEvent('pointerdown',{clientX:100,clientY:100}));
        el('view').dispatchEvent(new w.MouseEvent('pointermove',{clientX:150,clientY:125}));
        assert.notEqual(w.eval('yaw'),.1);
        el('view').dispatchEvent(new w.MouseEvent('pointerup'));
        el('reset').click();assert.equal(JSON.stringify(w.eval('[yaw,pitch,fov]')),'[0,0,90]');
        el('flat').click();assert.equal(el('flat').getAttribute('aria-pressed'),'true');
        state.video_frames++;await loop('stateLoop');await loop('videoLoop');
        assert.equal(bitmapClosed,1);
    } else {
        assert.equal(el('view').hidden,true);
        assert.equal(w.getComputedStyle(el('view')).display,'none');
        assert.equal(el('flatPreview').hidden,false);
        assert.equal(w.getComputedStyle(el('flatPreview')).objectFit,'contain');
        assert.equal(el('flatPreview').src,'blob:test-frame');
        assert.equal(el('pano').disabled,true);
        assert.equal(el('reset').disabled,true);
    }
    el('fullscreen').click();assert.ok(calls.some(x=>x[0]==='fullscreen'));
    state.healthy=false;state.status='failed';state.error='SDR RX OVERFLOW; continuity lost';
    state.video_age_s=4;state.mer_age_s=4;state.tx={fill_slack_us:24.5};state.tx_age_s=4;
    await loop('stateLoop');
    assert.equal(el('connection').textContent,'FAILED');
    assert.equal(el('videoLabel').textContent,'VIDEO STALE / WAITING');
    assert.equal(el('mer').textContent,'—');
    assert.equal(el('txState').textContent,'stale');
    assert.equal(el('error').textContent,state.error);
    w.fetch=async()=>{throw Error('test disconnected');};await loop('stateLoop');
    assert.equal(el('connection').textContent,'DISCONNECTED');
    assert.ok(el('error').textContent.includes('test disconnected'));
    assert.deepEqual(errors,[]);
    if (process.env.RX_SCREENSHOTS) {
        fs.mkdirSync(process.env.RX_SCREENSHOTS,{recursive:true});
        for (const [id,surface] of surfaces)
            fs.writeFileSync(path.join(process.env.RX_SCREENSHOTS,id+'.png'),surface.toBuffer('image/png'));
    }
    dom.window.close();
}
(async()=>{
    await render(true);await render(false);
    console.log('dashboard: DOM, CSS, spectrum/waterfall pixels, video calls, controls, stale/error states, flat fallback passed');
    console.log('static check only: GPU shader pixels and responsive layout still need a real browser');
})().catch(error=>{console.error(error);process.exitCode=1;});
