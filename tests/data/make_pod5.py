# Regenerates tests/data/tiny.pod5 (the tests only read the committed file):
#   pip install pod5 && python3 tests/data/make_pod5.py tests/data/tiny.pod5
# Three reads (UUIDs 0x1234..0x1236) with 50 / 60 / 70 samples of
# (i * (k + 1)) % 200 and one run-info record.
import sys, uuid, datetime
import numpy as np
import pod5
out = sys.argv[1]
run = pod5.RunInfo(
    acquisition_id="acq-0001", acquisition_start_time=datetime.datetime(2026, 10, 3, tzinfo=datetime.timezone.utc),
    adc_max=4095, adc_min=-4096, context_tags={"sequencing_kit": "sqk-toy"}, experiment_name="toy-run",
    flow_cell_id="FAX00001", flow_cell_product_code="FLO-TOY", protocol_name="toy_protocol",
    protocol_run_id="run-0001", protocol_start_time=datetime.datetime(2026, 10, 3, tzinfo=datetime.timezone.utc),
    sample_id="sample-1", sample_rate=5000, sequencing_kit="sqk-toy", sequencer_position="MN00001",
    sequencer_position_type="MinION", software="vv tests", system_name="toybox", system_type="toy",
    tracking_id={"run_id": "run-0001"})
reads = []
for k in range(3):
    sig = (np.arange(50 + 10 * k, dtype=np.int16) * (k + 1)) % 200
    reads.append(pod5.Read(
        read_id=uuid.UUID(int=0x1234 + k), end_reason=pod5.EndReason.from_reason_with_default_forced(pod5.EndReasonEnum.SIGNAL_POSITIVE),
        calibration=pod5.Calibration(offset=-240.0, scale=0.15),
        pore=pod5.Pore(channel=10 + k, well=1, pore_type="toy_pore"),
        read_number=100 + k, start_sample=1000 * k, median_before=200.5 + k,
        run_info=run, signal=sig))
with pod5.Writer(out) as w:
    w.add_reads(reads)
