const fs = require('fs');
const validator = require('./gltf-check/node_modules/gltf-validator');
(async()=>{
 for (const file of process.argv.slice(2)) {
  const result=await validator.validateBytes(new Uint8Array(fs.readFileSync(file)),{uri:file,maxIssues:1000});
  fs.writeFileSync(file+'.validation.json',JSON.stringify(result,null,2));
  console.log(JSON.stringify({file,errors:result.issues.numErrors,warnings:result.issues.numWarnings,messages:result.issues.messages.slice(0,12)}));
  if(result.issues.numErrors)process.exitCode=1;
 }
})();
