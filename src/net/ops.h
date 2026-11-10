#ifndef OPS_H
#define OPS_H

#include <math.h>
#include <string.h>

#define OPS_EPS 1e-5f

// 1. 线性层 (全连接层)
// 输入: x [in_dim], W [out_dim * in_dim], b [out_dim]
// 输出: out [out_dim]
// 公式: out = W * x + b
void linear(const float *x, const float *W, const float *b, float *out, int in_dim, int out_dim)
{
    for (int i = 0; i < out_dim; ++i)
    {
        float sum = b[i];
        for (int j = 0; j < in_dim; ++j)
            sum += x[j] * W[i * in_dim + j];  // 注意PyTorch的W是[out, in]排列
    
        out[i] = sum;
    }
}

// 2. ReLU 激活 （原地操作）
void relu_inplace(float *x, int dim)
{
    for (int i = 0; i < dim; ++i)
    {
        if (x[i] < 0.0f) x[i] = 0.0f;
    }
}

// 3. LayerNorm
// x, out 维度为 dim， gamma (weight) 和 beta (bias) 也是 dim
void layernorm(const float *x, const float *gamma,
               const float *beta, float *out, int dim)
{
    float mean = 0.0f;
    for (int i = 0; i < dim; ++i)
        mean += x[i];
    mean /= dim;

    float var = 0.0f;
    for (int i = 0; i < dim; ++i)
    {
        float diff = x[i] - mean;
        var += diff * diff;
    }
    var /= dim; 
    // PyTorch LayerNorm 默认除以dim，不是 dim−1

    // 防止除0
    float std_inv = 1.0f / sqrtf(var + OPS_EPS);

    for (int i = 0; i < dim; ++i)
        out[i] = (x[i] - mean) * std_inv * gamma[i] + beta[i];
}

// 4. Softmax (原地操作，带防溢出机制)
void softmax_inplace(float *x, int dim)
{
    float max_val = x[0];
    for (int i = 1; i < dim; ++i)
    {
        if (x[i] > max_val) max_val = x[i];
    }

    float sum = 0.0f;
    for (int i = 0; i < dim; ++i)
    {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }

    for (int i = 0; i < dim; ++i)
        x[i] /= sum;
}

void add_vec(const float *a, const float *b, float *out, int dim)
{
    for (int i = 0; i < dim; ++i)
        out[i] = a[i] + b[i];
}

// 6. Embedding 查表 (用于你的 type_emb)
// 直接将权重表中对应 token 的向量拷贝出来
void embedding_lookup(const float *table, int token_id, float *out, int dim) {
    memcpy(out, table + token_id * dim, dim * sizeof(float));
}


#endif // OPS.h