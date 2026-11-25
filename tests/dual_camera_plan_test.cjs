// node --test tests/dual_camera_plan_test.cjs; load the actual ArkTS query with a camera-kit fixture.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { stripTypeScriptTypes } = require('node:module');
const src=readFileSync(join(__dirname,'../entry/src/main/ets/util/DualCamera.ets'),'utf8')
  .replace("import { camera } from '@kit.CameraKit';",'').replace("import { common } from '@kit.AbilityKit';",'').replace(/^export /gm,'');
const load = new Function('camera',stripTypeScriptTypes(src)+'\nreturn queryDualCamera;');
const profile=(w,h,fmt=1003)=>({size:{width:w,height:h},format:fmt});
const device=id=>({cameraId:id});
const info=(id,type,profiles)=>({device:device(id),type,modes:[1,2],outputCapabilities:[{previewProfiles:[profile(800,600)]},{previewProfiles:profiles}]});
function fixture(infos) {
  const calls=[];
  const manager={getCameraDevice(pos){return device(pos==='F'?'front':'rear');},
    getCameraConcurrentInfos(devices){calls.push(devices.map(d=>d.cameraId));return infos;}};
  const fn=load({getCameraManager:()=>manager,CameraType:{CAMERA_TYPE_DEFAULT:0},CameraPosition:{CAMERA_POSITION_BACK:'B',CAMERA_POSITION_FRONT:'F'},
    CameraFormat:{CAMERA_FORMAT_YUV_420_SP:1003},SceneMode:{NORMAL_VIDEO:2},CameraConcurrentType:{CAMERA_FULL_CAPABILITY:1,CAMERA_LIMITED_CAPABILITY:0}});
  return {calls,probe:(front,w,h)=>fn({},front,w,h)};
}
test('device and scene mapping, independent limited/full and distinct preview sizes',()=>{
  const s=fixture([info('front',0,[profile(640,480,2000),profile(320,240)]),info('rear',1,[profile(1280,720)])]);
  assert.deepEqual(s.probe(false,1280,720),{primaryId:'rear',secondaryId:'front',primaryType:1,secondaryType:0,
    width:1280,height:720,secondaryWidth:320,secondaryHeight:240});
  assert.deepEqual(s.calls,[['rear','front']]);
  assert.equal(s.probe(true,320,240).secondaryId,'rear');
  assert.equal(s.probe(false,800,600),undefined); // only available in another mode
});
test('empty pairs, duplicate or absent ID, malformed profiles and unknown type fail closed',()=>{
  const a=info('rear',1,[profile(1280,720)]), b=info('front',0,[profile(320,240)]);
  for(const pair of [[],[a],[a,a],[a,{...b,type:9}],[a,{...b,modes:[1]}],
    [a,{...b,modes:[1,3]}],[a,info('front',1,[profile(321,240),profile(640,480,2000)])],
    [info('rear',1,[profile(8192,4096)]),b]]) assert.equal(fixture(pair).probe(false,1280,720),undefined);
});
