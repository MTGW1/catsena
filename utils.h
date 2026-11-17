#include "mapdef.h"
#include "map.h"
// 其他工具

// 1. 选定(框选区域内中存在邻节点/父节点/子节点不在框选区域内的节点)的函数 输出Node.uuid列表
//     即寻找选定节点组里的根节点(组)
// 输入: 
//  - m: 图实例
//  - selected_nodes: 选定节点数组
//  - selected_count: 选定节点数量
// 输出:
//  - out_uuids: 输出的节点uuid数组(由函数分配,调用方负责释放)
//  - out_count: 输出的节点数量
//  - out_outside_uuids: 输出的外部节点uuid数组(由函数分配,调用方负责释放)
//  - out_outside_count: 输出的外部节点数量
// 返回: 0 成功, -1 失败
int FindSelectedNodesWithOutsideConnections(
    map*         m, 
    const node** selected_nodes, 
    size_t       selected_count, 
    char***      out_uuids, 
    size_t*      out_count, 
    char***      out_outside_uuids, 
    size_t*      out_outside_count
);

// 2. 这两个节点可能认识吗? (这两个节点在一张子图里吗?)
// 输入:
//  - m: 图实例
//  - node_a: 节点A
//  - node_b: 节点B
// 返回: 1 认识, 0 不认识, -1 错误
int IsNodesMayKnowEachOther(map* m, const node* node_a, const node* node_b);    

// 3. 我超喂我的邻居/子节点/父节点的uuid呢?
// 输入:
//  - n: 节点
//  - mode: 0=邻居, 1=子节点, 2=父节点, 3=all
//          4=0+1 , 5=0+2, 6=1+2, 7=0+1+2
// 输出:
//  - out_uuids: 输出的节点uuid数组(由函数分配,调用方负责释放)
//  - out_count: 输出的节点数量
// 返回: 0 成功, -1 失败
int GetNodeRelatedUUIDs(const node* n, int mode, char*** out_uuids, size_t* out_count);
