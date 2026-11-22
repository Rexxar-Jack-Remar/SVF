#!/usr/bin/env python3
"""Compile analysis inputs only. Exercise both live WPA and standalone snapshot replay."""
import argparse
import json
import pathlib
import subprocess
import tempfile

DECL = """
#include <stddef.h>
extern void free(void *);
extern void *malloc(size_t);
extern long read(int, void *, unsigned long);
extern int puts(const char *);
extern int getchar(void);
extern int printf(const char *, ...);
extern void *memcpy(void *, const void *, size_t);
"""
CASES = {
    "uaf": {
        "after": ("void release(void *p){ free(p); } int main(){char *p=malloc(8); release(p); return *p;}", True),
        "before": ("void release(void *p){ free(p); } int main(){char *p=malloc(8); int v=*p; release(p); return v;}", False),
        "reassign": ("int main(){char *p=malloc(8), *q=p; free(p); p=malloc(8); *p=1; return *q;}", True),
        "distinct": ("int main(){char *p=malloc(8), *q=malloc(8); free(p); return *q;}", False),
        "loop": ("int main(int n,char**v){char *p=malloc(8); int x=0;while(n--){x+=*p;free(p);}return x;}", True),
        "indirect": ("void release(void*p){free(p);} void (*volatile fn)(void*)=release; int main(){char *p=malloc(8);fn(p);return *p;}", True),
        "field": ("struct S{int a,b;}; int main(){struct S*p=malloc(sizeof *p);free(p);return p->b;}", True),
    },
    "taint": {
        "copy": ("void cp(char*d,char*s){memcpy(d,s,8);} int main(){char a[8],b[8];read(0,a,8);cp(b,a);return puts(b);}", True),
        "direct": ("int main(){char a[8]; read(0,a,8);return puts(a);}", True),
        "distinct": ("int main(){char a[8],b[8]={0}; read(0,a,8);return puts(b);}", False),
        "before": ("int main(){char a[8]={0};int n=puts(a);read(0,a,8);return n;}", False),
        "nosource": ("int main(){return puts(\"constant\");}", False),
        "scalar": ("int id(int x){return x+1;} int main(int n,char**v){int x=getchar(); volatile int a=n?id(x):0;return printf(\"%d\",a);}", True),
    }
}

def run(args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{args}\n{result.stderr[-4000:]}\n{result.stdout[-1000:]}")
    return result

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wpa", required=True)
    ap.add_argument("--llvm-bin", required=True, type=pathlib.Path)
    ap.add_argument("--pta", default="ander", choices=["ander","vfspta"])
    ap.add_argument("--backends", nargs="+", default=["cpu"])
    cfg = ap.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        base = pathlib.Path(folder)
        c, raw, bc, snap, report = (base/name for name in ["case.c","raw.bc","case.bc","snap","report"])
        for client, cases in CASES.items():
            for name,(body,positive) in cases.items():
                c.write_text(DECL + body)
                run([cfg.llvm_bin/"clang","-g","-O0","-Xclang","-disable-O0-optnone","-emit-llvm","-c",c,"-o",raw])
                run([cfg.llvm_bin/"opt","-passes=mem2reg",raw,"-o",bc])
                opts = [cfg.wpa,f"-alias-client={client}",f"-client-report={report}","-client-verify"]
                run(opts + [f"-{cfg.pta}",f"-client-save={snap}",bc])
                reference = report.read_text()
                records = [json.loads(line) for line in reference.splitlines()]
                assert bool(records) == positive, (client,name,records)
                if client == "uaf":
                    for r in records:
                        for _,to,end in r["uses"]:
                            assert to and -1 <= end < len(r["blocks"])
                            while end >= 0:
                                prev,block,label = r["blocks"][end]
                                assert -1 <= prev < end and block >= 0 and label
                                end = prev
                else:
                    assert all(r["path"][0] == r["from"] and r["path"][-1] == r["to"] for r in records)
                for backend in cfg.backends:
                    for tile in [1,7]:
                        replay = [f"-client-backend={backend}",f"-client-tile={tile}",f"-client-load={snap}"]
                        if backend in ("bitmap","pestrie"):
                            replay.append(f"-client-index-cache={base / 'cache'}")
                        metrics = run(opts+replay)
                        if tile == 7 and backend in ("bitmap","pestrie"):
                            parsed = json.loads(next(line.removeprefix("CLIENT_METRICS ")
                                for line in metrics.stderr.splitlines() if line.startswith("CLIENT_METRICS ")))
                            assert parsed["baseline_index"] in ("loaded","none")
                        assert report.read_text() == reference, (client,name,backend,tile)
                if name == "reassign":
                    uses = [u for record in records for u in record["uses"]]
                    assert len(uses) == 1 and "/load" in uses[0][1], uses
                print(client,name,len(records),flush=True)
        # Cloned library bodies may change insertion order across fresh WPA processes.
        # Reorder named functions while preserving all instructions and call edges.
        helpers = [
            "void release(char *p){free(p);}",
            "int access_byte(char *p){return *p;}",
            "void input(char *p){read(0,p,8);}",
            "int output(char *p){return puts(p);}",
        ]
        main_body = "int main(){char *p=malloc(8);input(p);int x=output(p);release(p);return x+access_byte(p);}"
        expected = {}
        for sequence in (helpers, list(reversed(helpers))):
            c.write_text(DECL + "\n".join(sequence) + "\n" + main_body)
            run([cfg.llvm_bin/"clang","-O0","-emit-llvm","-c",c,"-o",bc])
            for client in CASES:
                run([cfg.wpa,f"-{cfg.pta}",f"-alias-client={client}",f"-client-report={report}",bc])
                actual = report.read_bytes()
                assert actual and expected.setdefault(client, actual) == actual, (client,"function order")
        print("stable function-order reports", flush=True)
        snap.write_bytes(snap.read_bytes()[:100])
        broken = subprocess.run([cfg.wpa, "-alias-client=uaf", f"-client-load={snap}"],capture_output=True)
        assert broken.returncode != 0, "truncated snapshot was accepted"

if __name__ == "__main__":
    main()
