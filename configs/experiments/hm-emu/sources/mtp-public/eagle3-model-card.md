---
license: other
base_model: moonshotai/Kimi-K3
tags:
- text-generation
- speculative-decoding
- eagle3
- kimi-k3
- torchspec
---

# kimi-k3-eagle3-mla

## Model Overview

kimi-k3-eagle3-mla is an Eagle3 MTP draft model with MLA (Multi-Latent Attention) for accelerating inference of [Kimi-K3](https://huggingface.co/moonshotai/Kimi-K3), trained with [TorchSpec](https://github.com/torchspec-project/TorchSpec) — an online speculative decoding training framework that runs FSDP training and inference concurrently. If you find this draft model useful, please give our project TorchSpec a star on [GitHub](https://github.com/torchspec-project/TorchSpec).

### Why an MLA (Multi-Latent Attention) Draft Model

Compared with an MHA draft model, the MLA variant is a better fit for Kimi-K3 deployment:

- Uses less KV cache, which reduces serving memory pressure.
- Matches Kimi-K3's MLA architecture, so it fits more naturally into the inference engine's KV-cache handling under different serving scenarios such as PD-Disaggregation.

## Performance

The primary metric is accept_length - the average number of tokens accepted per speculation step with `topk=1, num_steps=3, num_draft_tokens=4`. Higher is better; the ceiling for this configuration is 4.

Measured against an [NVFP4 Kimi-K3](https://huggingface.co/nvidia/Kimi-K3-NVFP4) target on GB300 (TP=8) served with TokenSpeed, at `temperature=1.0, top_p=0.95`, reasoning effort max. Acceptance is attributed per request from the scheduler log, so N is the number of requests measured.

| Category | Benchmark | N | Acc Len |
| --- | --- | --- | --- |
| Dialogue | [MTBench](https://github.com/lm-sys/FastChat/tree/main/fastchat/llm_judge) | 160 | 2.794 |
| Chinese | [CEval](https://huggingface.co/datasets/ceval/ceval-exam) | 1346 | 2.397 |
| Multilingual | [SPEED-Bench](https://huggingface.co/datasets/nvidia/SPEED-Bench) multilingual | 80 | 2.932 |
| Math | [GSM8K](https://huggingface.co/datasets/openai/gsm8k) | 1319 | 3.556 |
| Math | [MATH500](https://huggingface.co/datasets/HuggingFaceH4/MATH-500) | 500 | 3.097 |
| Math | [AIME 2026](https://huggingface.co/datasets/MathArena/aime_2026) | 30 | 2.208 |
| Code | [HumanEval](https://huggingface.co/datasets/openai/openai_humaneval) | 164 | 3.291 |
| Code | [SPEED-Bench](https://huggingface.co/datasets/nvidia/SPEED-Bench) coding | 89 | 3.089 |
| RAG | [SPEED-Bench](https://huggingface.co/datasets/nvidia/SPEED-Bench) rag | 91 | 3.017 |
| QA | [SPEED-Bench](https://huggingface.co/datasets/nvidia/SPEED-Bench) qa | 80 | 2.710 |
| Writing | [SPEED-Bench](https://huggingface.co/datasets/nvidia/SPEED-Bench) writing | 84 | 2.447 |

---

## Quick Start

### Requirements

- NVIDIA GPU with CUDA 12.0+
- [TokenSpeed](https://github.com/lightseekorg/tokenspeed), or [vLLM](https://github.com/vllm-project/vllm) >= 0.20.0

### Launch Server (TokenSpeed)

This is the configuration the acceptance numbers above were measured on.

```bash
python3 -m tokenspeed.cli serve /models/Kimi-K3 \
  --served-model-name nvidia/Kimi-K3-NVFP4 \
  --attn-tp-size 8 --moe-tp-size 8 --mm-encoder-tp-mode data \
  --max-model-len 80000 --max-num-seqs 16 \
  --gpu-memory-utilization 0.92 --trust-remote-code \
  --attention-backend tokenspeed_mla --kda-backend cutedsl_kda \
  --moe-backend flashinfer_trtllm --kv-cache-dtype fp8 \
  --speculative-algorithm EAGLE3 \
  --speculative-draft-model-path /models/eagle3 \
  --speculative-num-steps 3 --speculative-eagle-topk 1 \
  --speculative-num-draft-tokens 4 \
  --speculative-draft-model-quantization unquant \
  --sampling-backend flashinfer \
  --drafter-attention-backend tokenspeed_mla \
  --max-cudagraph-capture-size 16 --cudagraph-capture-sizes 1 2 4 8 16 \
  --host 0.0.0.0 --port 8000
```

`/models/Kimi-K3` is the target checkpoint and `/models/eagle3` is this draft; point both at your local paths.

`--sampling-backend flashinfer` keeps rejection sampling faithful to the requested `temperature` / `top_p`, which is required to reproduce the acceptance numbers above.

### Launch Server (vLLM)

```bash
vllm serve moonshotai/Kimi-K3 \
    --tensor-parallel-size 8 \
    --speculative-config '{"model": "lightseekorg/kimi-k3-eagle3-mla", "method": "eagle3", "num_speculative_tokens": 3}' \
    --trust-remote-code
```

### Launch Server (SGLang)

```bash
sglang serve \
  --model-path moonshotai/Kimi-K3 \
  --tp 8 \
  --trust-remote-code \
  --reasoning-parser kimi_k2 \
  --tool-call-parser kimi_k2 \
  --speculative-algorithm EAGLE3 \
  --speculative-num-steps 3 \
  --speculative-eagle-topk 1 \
  --speculative-num-draft-tokens 4 \
  --speculative-draft-model-path lightseekorg/kimi-k3-eagle3-mla
```

## Citation

```bibtex
@misc{torchspec2026,
  title={TorchSpec: An Online Speculative Decoding Training Framework},
  url={https://github.com/torchspec-project/TorchSpec},
  year={2026}
}
```
