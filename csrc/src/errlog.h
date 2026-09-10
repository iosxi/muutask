#pragma once

#include <string>

// 転んだときだけ書き残す、小さなログ。
//
// 常駐アプリなので標準エラー出力は誰も見ていない (配布用の exe はコンソールを
// 持たない)。「表示が固まったまま戻らない」ように後から原因を追えないと困る
// 不具合があるため、想定外の失敗だけをファイルに残す。
//
// 置き場所は config.json と同じ (exe と同じフォルダー)。やめるときはフォルダー
// ごと消せば痕跡が残らない、という方針を崩さないため。放っておくと際限なく
// 育つので上限を決めて、超えたら捨てて書き直す。
namespace errlog {

/// 1 件書き残す。書けない場所に置かれていたら黙って諦める。
void Write(std::wstring const& message);

/// いま捕まえている例外の説明を添えて書き残す。
void Exception(std::wstring const& message, std::wstring const& detail);

/// ログ ファイルの場所 (exe と同じフォルダーの MuuTask.log)。
std::wstring LogPath();

}  // namespace errlog
