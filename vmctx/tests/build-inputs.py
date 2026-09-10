#!/usr/bin/env python3
"""Exercise build freshness in an isolated tree with real compiled executables."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix='vmctx-build-inputs-') as tmp:
        root = Path(tmp)
        user = root / 'user'
        user.mkdir()
        (root / 'kernel').mkdir()
        shutil.copyfile(Path(__file__).resolve().parents[1] / 'user/Makefile', user / 'Makefile')
        (user / 'own.h').write_text('#define TEST_VALUE 1\n')
        source = '''#include <stdio.h>
#include "own.h"
#ifndef __NR_vmctx_run
#define __NR_vmctx_run 470
#endif
#ifndef __NR_vmctx_ctl
#define __NR_vmctx_ctl 471
#endif
#ifndef TEST_OPTION
#define TEST_OPTION 0
#endif
int main(void) {
    printf("%d %d %d %d %s\\n", TEST_VALUE, TEST_OPTION,
           __NR_vmctx_run, __NR_vmctx_ctl, VMCTX_SRC_ID);
    return 0;
}
'''
        for tool in ('vmhome', 'vmremote'):
            (user / (tool + '.c')).write_text(source)

        def make(*options, ok=True):
            run = subprocess.run(['make', '-s', '-j3', '../build/vmhome',
                                  '../build/vmremote', '../build/vmremote-local', *options],
                                 cwd=user, text=True, capture_output=True)
            assert (run.returncode == 0) == ok, run.stdout + run.stderr
            return run.stdout

        def output(tool='vmhome'):
            return subprocess.check_output([root / 'build' / tool], text=True).split()

        def identities():
            return {p.name: (p.stat().st_mtime_ns, p.read_bytes())
                    for p in (root / 'build').iterdir()}

        make()
        first = identities()
        assert output()[:4] == ['1', '0', '470', '471']
        assert output('vmremote-local')[2:4] == ['472', '473']
        assert not make(), 'unchanged inputs must not rebuild'
        assert identities() == first

        header = user / 'own.h'
        times = (header.stat().st_atime_ns, header.stat().st_mtime_ns)
        header.write_text('#define TEST_VALUE 2\n')
        os.utime(header, ns=times)
        make()
        assert output()[0] == '2', 'preserved mtime must not hide a header edit'
        second_id = output()[-1]
        header.write_text('#define TEST_VALUE 1\n')
        os.utime(header, ns=times)
        make()
        assert output()[0] == '1' and output()[-1] != second_id

        make('CFLAGS=-O0 -DTEST_OPTION=7')
        assert output()[1] == '7'
        assert output('vmremote-local')[2:4] == ['472', '473'], 'CFLAGS must retain the ABI'
        make()
        assert output()[1] == '0', 'restoring flags must rebuild'
        valid = identities()
        header.write_text('#error intentional failed rebuild\n')
        make(ok=False)
        assert identities() == valid, 'failed build replaced a valid executable or identity'
        header.write_text('#define TEST_VALUE 1\n')
        make()
        assert identities() == valid, 'restored content should reuse the valid build'

        binary = root / 'build/vmhome'
        binary.unlink()
        make()
        assert output()[0] == '1', 'a surviving identity must not hide a missing executable'
        assert not list((root / 'build').glob('*.tmp.*')), 'build scratch files leaked'
        print('PASS: header changes, rollback, flags, ABI, no-op, failure atomicity, missing output')


if __name__ == '__main__':
    main()
