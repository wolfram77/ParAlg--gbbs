const fs = require('fs');
const os = require('os');
const path = require('path');
const readline = require('readline');




//#region FILE
// -----------

// Read file as text.
function readFile(pth) {
  var d = fs.readFileSync(pth, 'utf8');
  return d.replace(/\r?\n/g, '\n');
}

// Write text to file.
function writeFile(pth, d) {
  d = d.replace(/\r?\n/g, os.EOL);
  fs.writeFileSync(pth, d);
}
//#endregion




//#region HEADER LINES
// --------------------

// Count the number of header lines in a MatrixMarket file.
async function headerLines(pth) {
  var a  = 0;
  var rl = readline.createInterface({input: fs.createReadStream(pth)});
  for await (var line of rl) {
    if (line[0]==='%') ++a;
    else break;
  }
  return a+1;  // +1 for the row/column count line
}




//#region MAIN
// -----------

async function main(cmd, inp, out) {
  switch (cmd) {
    case 'header-lines':
      var lines = await headerLines(inp);
      console.log(lines);
      break;
    default:
      console.error(`error: "${cmd}"?`);
      break;
  }
}
main(...process.argv.slice(2));
