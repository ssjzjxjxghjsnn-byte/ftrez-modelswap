
import argparse
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--unity', default='E:/Unity/2022.3.62f3/Editor/Unity.exe')
parser.add_argument('--project', default='build/unity-eevee')
parser.add_argument('--assets', default=str(Path(__file__).resolve().parent / 'Assets'))
args = parser.parse_args()
project = Path(args.project).resolve()
(project / 'Packages').mkdir(parents=True, exist_ok=True)
(project / 'ProjectSettings').mkdir(exist_ok=True)
(project / 'Packages/manifest.json').write_text(json.dumps({'dependencies': {
    'com.unity.render-pipelines.universal': '14.0.12',
    'com.unity.modules.assetbundle': '1.0.0',
    'com.unity.modules.imageconversion': '1.0.0',
    'com.unity.modules.jsonserialize': '1.0.0'
}}, indent=2), encoding='utf-8')
(project / 'ProjectSettings/ProjectVersion.txt').write_text(
    'm_EditorVersion: 2022.3.62f3' + chr(10), encoding='utf-8')
shutil.copytree(Path(args.assets), project / 'Assets', dirs_exist_ok=True)
log = project.parent / 'unity-eevee-build.log'
subprocess.run([args.unity, '-batchmode', '-nographics', '-projectPath', str(project),
    '-buildTarget', 'Android', '-executeMethod', 'BuildFTRezToon.Build', '-quit',
    '-logFile', str(log)], check=True)
print(project / 'Build/Android/ftrez-toon.bundle')
