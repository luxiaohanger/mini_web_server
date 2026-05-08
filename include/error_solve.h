#pragma once

// 异常检测
// 当被检测函数的返回值只表示成功与否，而不需要额外保存时
// 直接写成 errif(func() == -1 , "errmsg");
void errif(bool condition, const char* errmsg);