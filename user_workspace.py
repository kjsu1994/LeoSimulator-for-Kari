# SPDX-License-Identifier: GPL-3.0-only
"""Packaged application storage and diagnostics."""
import hashlib
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import tempfile

logger = logging.getLogger('leo')


def root_path():
    return Path(os.environ['LOCALAPPDATA'])/'LEO Network Simulator'


def location_profile(assets):
    key=hashlib.sha256(os.path.normcase(str(assets.resolve())).encode()).hexdigest()[:24]
    return root_path()/'locations'/key


def select_results(assets, profile):
    preference=profile/'results.json'
    if preference.exists():
        try:
            chosen=Path(json.loads(preference.read_bytes())['path'])
            if chosen.is_absolute() and chosen.is_dir():
                return chosen,''
        except (OSError,ValueError,KeyError,TypeError):
            pass
        return assets/'data','선택한 결과 폴더를 열 수 없습니다. 결과 폴더를 다시 선택하세요.'
    return assets/'data',''


def initialize_results(directory):
    directory.mkdir(parents=True,exist_ok=True)
    for path in (directory,directory/'runs',directory/'experiments'):
        path.mkdir(exist_ok=True)
        with tempfile.TemporaryFile(dir=path):
            pass


def save_result_choice(profile, assets, current, target):
    from windows_runtime import workspace_lock
    preference=profile/'results.json'
    if target is None:
        preference.unlink(missing_ok=True)
        return
    target=target.resolve()
    if target==assets or any(target.is_relative_to(assets/name) for name in ('settings','engine','manual','logs','data/runs','data/experiments')) or any(target.is_relative_to(current/name) for name in ('runs','experiments')):
        raise ValueError('결과만 저장할 별도 폴더를 선택하세요. 설치 폴더를 쓰려면 기본 위치로를 누르세요.')
    if not target.is_dir():
        raise ValueError('저장할 폴더를 먼저 만드세요.')
    if target!=current:
        with workspace_lock(target):
            initialize_results(target)
    else:
        initialize_results(target)
    temporary=preference.with_suffix('.tmp')
    try:
        temporary.write_text(json.dumps({'path':str(target)}),encoding='utf-8')
        temporary.replace(preference)
    finally:
        temporary.unlink(missing_ok=True)


def setup_logging(root):
    directory=root/'logs'
    directory.mkdir(parents=True,exist_ok=True)
    handler=RotatingFileHandler(directory/'app.log',maxBytes=2*1024*1024,backupCount=3,encoding='utf-8')
    handler.setFormatter(logging.Formatter('%(asctime)s %(levelname)s %(message)s'))
    logger.setLevel(logging.INFO)
    logger.addHandler(handler)
    return handler
