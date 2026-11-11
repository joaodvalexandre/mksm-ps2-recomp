"""Reference side of the decoder self-test of tools/vu1gen (development only; Python is not a build prerequisite).

    python -I tools/vu1gen/selftest/vudis_listing.py <folder with vudis.py> <game ELF> <output folder>

vudis.py is the Python decoder of the study the generator was ported from (it mirrors the decoding and the usage
tables of upstream's VU1 interpreter). For each of its images this writes, like `vu1gen --listing`:

    listing_<image>.txt   one line per pair, the format of the study's raw listings
    usage_<image>.txt     three lines per pair with every field of the decoder

The self-test is that the two tools write the same bytes (PROGRESS.md has the commands). The output quotes the
game's microcode: the output folder must be outside the repository.
"""
import os
import sys


def raw_listing(vudis, name, pairs):
    address, own = vudis.PROGRAMS[name]
    out = []
    out.append("; VU1 program '%s': ELF 0x%X, image of 2048 pairs, %d pairs before the next image" % (name, address, own))
    out.append("; addr  pair  lower    upper    bits  | upper instruction                         | lower instruction")
    for p in pairs:
        mark = ""
        if p.reserved:
            mark += " <RESERVED>"
        if p.shadow_reg:
            mark += " <shadow vf%02d%s>" % (p.shadow_reg, " lower-write-dropped" if p.suppressed_lower_vf else "")
        out.append("%04X  %4d  %08X %08X %s | %-40s | %s%s" % (
            p.pc, p.index, p.lower_word, p.upper_word, p.bits(), p.upper.text, p.lower.text, mark))
        if p.index == own - 1:
            out.append("; ---- end of the program's own pairs (the next image starts here in the ELF) ----")
    return "\n".join(out) + "\n"


def optional(value):
    return "-" if value is None else "%d" % value


def instr_line(vudis, tag, ins):
    vf_reads = ",".join("%d:%X" % (reg, lanes) for reg, lanes in ins.vf_reads)
    vf_write = "%d:%X" % ins.vf_write if ins.vf_write else "-"
    memory = "%s,%d,%d,%X,%d" % ins.mem if ins.mem else "-"
    raw = ",".join("%s=%d" % (key, ins.raw_fields[key]) for key in sorted(ins.raw_fields))
    return ("  %s %s reserved=%d nop=%d"
            " | vfr=[%s] vfw=%s vir=%04X viw=%04X accr=%X accw=%X pipe=%s lat=%d vflat=%d vilat=%d waitq=%d waitp=%d"
            " rclipu=%d wclipu=%d delay=%d"
            " | dest=%X rq=%d rp=%d ri=%d rr=%d wr=%d wq=%d wp=%d rstatus=%d rmac=%d rclip=%d wflags=%d wclip=%d"
            " wsticky=%d"
            " branch=%s rel=%s link=%s cond=%d jr=%s mem=%s kick=%s xtop=%d xitop=%d"
            " | raw=[%s]") % (
        tag, ins.mnem, ins.reserved, ins.nop,
        vf_reads, vf_write, ins.vi_reads, ins.vi_write, ins.acc_read, ins.acc_write, vudis.PIPE_NAMES[ins.pipeline],
        ins.latency, ins.vf_latency, ins.vi_latency, ins.waitq, ins.waitp,
        ins.reads_clip_usage, ins.writes_clip_usage, ins.delays_branch,
        ins.dest, ins.reads_q, ins.reads_p, ins.reads_i, ins.reads_r, ins.writes_r, ins.writes_q, ins.writes_p,
        ins.reads_status, ins.reads_mac, ins.reads_clip, ins.writes_macstatus, ins.writes_clip,
        ins.writes_sticky,
        ins.branch or "-", optional(ins.target_rel), optional(ins.link), ins.cond, optional(ins.jr_reg), memory,
        optional(ins.xgkick_reg), ins.xtop, ins.xitop,
        raw)


def usage_listing(vudis, pairs):
    out = []
    for p in pairs:
        out.append("%04X %08X %08X %s shadow=%d suppressed=%d reserved=%d" % (
            p.pc, p.lower_word, p.upper_word, p.bits(), p.shadow_reg, p.suppressed_lower_vf, p.reserved))
        out.append(instr_line(vudis, "U", p.upper))
        out.append(instr_line(vudis, "L", p.lower))
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    study, elf_path, out_dir = sys.argv[1:4]
    sys.path.insert(0, study)
    import vudis

    with open(elf_path, "rb") as fh:
        elf = fh.read()
    os.makedirs(out_dir, exist_ok=True)
    for name in vudis.PROGRAMS:
        pairs = vudis.decode_program(vudis.load_image(name, elf))
        with open(os.path.join(out_dir, "listing_%s.txt" % name), "w", newline="\n") as fh:
            fh.write(raw_listing(vudis, name, pairs))
        with open(os.path.join(out_dir, "usage_%s.txt" % name), "w", newline="\n") as fh:
            fh.write(usage_listing(vudis, pairs))
        print("%s: %d pairs" % (name, len(pairs)))


if __name__ == "__main__":
    main()
