﻿#include "move_picker.h"
#include "thread.h"

namespace {

// -----------------------
//   LVA
// -----------------------

// 被害が小さいように、LVA(価値の低い駒)を動かして取るほうが優先されたほうが良いので駒に価値の低い順に番号をつける。そのためのテーブル。
// ※ LVA = Least Valuable Aggressor。cf.MVV-LVA

static const Value LVATable[PIECE_WHITE] = {
  Value(0), Value(1) /*歩*/, Value(2)/*香*/, Value(3)/*桂*/, Value(4)/*銀*/, Value(7)/*角*/, Value(8)/*飛*/, Value(6)/*金*/,
  Value(10000)/*王*/, Value(5)/*と*/, Value(5)/*杏*/, Value(5)/*圭*/, Value(5)/*全*/, Value(9)/*馬*/, Value(10)/*龍*/,Value(11)/*成金*/
};
inline Value LVA(const Piece pt) { return LVATable[pt]; }
  
// -----------------------
//   指し手オーダリング
// -----------------------

// 指し手を段階的に生成するために現在どの段階にあるかの状態を表す定数
enum Stages: int {

	// -----------------------------------------------------
	//   王手がかっていない通常探索時用の指し手生成
	// -----------------------------------------------------

	MAIN_SEARCH,				// 置換表の指し手を返すフェーズ
	CAPTURES_INIT,				// (CAPTURESの指し手生成)
	GOOD_CAPTURES,				// 捕獲する指し手(CAPTURES_PRO_PLUS)を生成して指し手を一つずつ返す
	KILLER0,
	KILLER1,
	COUNTERMOVE,				// counter moveの指し手
	QUIET_INIT,					// (QUIETの指し手生成)
	QUIET,						// CAPTURES_PRO_PLUSで生成しなかった指し手を生成して、一つずつ返す。SEE値の悪い手は後回し。
	BAD_CAPTURES,				// 捕獲する悪い指し手(SEE < 0 の指し手だが、将棋においてそこまで悪い手とは限らないが…)

	// 将棋ではBAD_CAPTURESをQUIETSの前にやったほうが良いという従来説は以下の実験データにより覆った。
	//  r300, 2585 - 62 - 2993(46.34% R - 25.46)[2016/08/19]
	// b1000, 1051 - 43 - 1256(45.56% R - 30.95)[2016/08/19]

	// -----------------------------------------------------
	//   王手がかっているときの静止探索/通常探索の指し手生成
	// -----------------------------------------------------

	EVASION,					// 置換表の指し手を返すフェーズ
	EVASIONS_INIT,				// (EVASIONSの指し手を生成)
	ALL_EVASIONS,				// 回避する指し手(EVASIONS)を生成した指し手を一つずつ返す

	// -----------------------------------------------------
	//   通常探索のProbCutの処理のなかから呼び出される用
	// -----------------------------------------------------

	PROBCUT,					// 置換表の指し手を返すフェーズ
	PROBCUT_INIT,				// (PROBCUTの指し手を生成)
	PROBCUT_CAPTURES,			// 直前の指し手での駒の価値を上回る駒取りの指し手のみを生成するフェーズ

	// -----------------------------------------------------
	//   王手がかっていない静止探索時用の指し手生成
	// -----------------------------------------------------

	// 王手の指し手を生成する場合

	QSEARCH_WITH_CHECKS,		// 置換表の指し手を返すフェーズ
	QCAPTURES_1_INIT,			// (指し手生成)
	QCAPTURES_1,				// 捕獲する指し手
	QCHECKS,					// 王手となる指し手
	
	// 王手の指し手は生成しない場合

	QSEARCH_NO_CHECKS,			// 置換表の指し手を返すフェーズ
	QCAPTURES_2_INIT,			// (指し手生成)
	QCAPTURES_2,				// 残り生成フェーズ(共通処理)

	// recaptureの指し手のみを生成

	// 静止探索で深さ-2以降は組み合わせ爆発を防ぐためにrecaptureのみを生成する場合
	QSEARCH_RECAPTURES,			// 置換表の指し手を返すフェーズ
	QRECAPTURES,				// 最後の移動した駒を捕獲する指し手(RECAPTURES)を生成した指し手を一つずつ返す

};

// -----------------------
//   partial insertion sort
// -----------------------

// partial_insertion_sort()は指し手を与えられたlimitまで降順でソートする。
// limitよりも小さい値の指し手の順序については、不定。
// 将棋だと指し手の数が多い(ことがある)ので、数が多いときは途中で打ち切ったほうがいいかも。
// 現状、全体時間の7%程度をこの関数で消費している。
void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {

	for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
		if (p->value >= limit)
		{
			ExtMove tmp = *p, *q;
			*p = *++sortedEnd;
			for (q = sortedEnd; q != begin && *(q - 1) < tmp; --q)
				*q = *(q - 1);
			*q = tmp;
		}
}

// beginからendのなかでベストのスコアのものを先頭(begin)に移動させる。
Move pick_best(ExtMove* begin, ExtMove* end)
{
	std::swap(*begin, *std::max_element(begin, end));
	return *begin;
}
} // end of namespace

#if defined (MUST_CAPTURE_SHOGI_ENGINE)
void MovePicker::checkMustCapture()
{
	// このnodeで合法なcaptureの指し手が1手でもあれば、必ずcaptureしなければならない。
	bool inCheck = pos.in_check();
	endMoves = inCheck ? generateMoves<EVASIONS>(pos, moves) : generateMoves<CAPTURES>(pos,moves);
	for (auto it = moves; it != endMoves; ++it)
	{
		// 合法な指し手が一つ見つかったので以降、captureしか返してはならない。
		// capturesで生成した指し手はcapturesに決まっているのだが、このチェックのコストは
		// 知れてるので構わない。
		if (pos.capture(it->move) && pos.legal(it->move))
		{
			mustCapture = true;
			return;
		}
	}
	mustCapture = false;
}
#endif

// 指し手オーダリング器

// 通常探索から呼び出されるとき用。
MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh,
	const PieceToHistory** ch, Move cm, Move* killers_p)
	: pos(p), mainHistory(mh), contHistory(ch), countermove(cm),
	killers{ killers_p[0], killers_p[1] }, depth(d)
{
	// 通常探索から呼び出されているので残り深さはゼロより大きい。
	ASSERT_LV3(d > DEPTH_ZERO);

#if defined (MUST_CAPTURE_SHOGI_ENGINE)
	checkMustCapture();
#endif

	// 次の指し手生成の段階
	// 王手がかかっているなら回避手、かかっていないなら通常探索用の指し手生成
	stage = pos.in_check() ? EVASION : MAIN_SEARCH;

	// 置換表の指し手があるならそれを最初に返す。ただしpseudo_legalでなければならない。
	ttMove = ttm && pos.pseudo_legal_s<false>(ttm) ? ttm : MOVE_NONE;

	// 置換表の指し手がないなら、次のstageから開始する。
	stage += (ttMove == MOVE_NONE);
}

// 静止探索から呼び出される時用。
MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, Square recapSq)
	: pos(p), mainHistory(mh) {

#ifdef MUST_CAPTURE_SHOGI_ENGINE
	checkMustCapture();
#endif

	// 静止探索から呼び出されているので残り深さはゼロ以下。
	ASSERT_LV3(d <= DEPTH_ZERO);

	if (pos.in_check())
		stage = EVASION;

	else if (d > DEPTH_QS_NO_CHECKS)
		stage = QSEARCH_WITH_CHECKS;

	else if (d > DEPTH_QS_RECAPTURES)
		stage = QSEARCH_NO_CHECKS;

	else
	{
		stage = QSEARCH_RECAPTURES;
		recaptureSquare = recapSq;
		return;
	}

	// 歩の不成、香の2段目への不成、大駒の不成を除外
	ttMove = ttm && pos.pseudo_legal_s<false>(ttm) ? ttm : MOVE_NONE;

	// 置換表の指し手がないなら、次のstageから開始する。
	stage += (ttMove == MOVE_NONE);
}

// 通常探索時にProbCutの処理から呼び出されるの専用
// th = 枝刈りのしきい値
MovePicker::MovePicker(const Position& p, Move ttm, Value th)
	: pos(p), threshold(th) {

	ASSERT_LV3(!pos.in_check());

#if defined (MUST_CAPTURE_SHOGI_ENGINE)
	checkMustCapture();
#endif

	stage = PROBCUT;

	// ProbCutにおいて、SEEが与えられたthresholdの値以上の指し手のみ生成する。
	// (置換表の指しても、この条件を満たさなければならない)
	ttMove = ttm
		&& pos.pseudo_legal_s<false>(ttm)
		&& pos.capture(ttm)
		&& pos.see_ge(ttm, threshold) ? ttm : MOVE_NONE;

	// 置換表の指し手がないなら、次のstageから開始する。
	stage += (ttMove == MOVE_NONE);
}

// QUIET、EVASIONS、CAPTURESのスコアリング。似た処理なので一本化。
template<MOVE_GEN_TYPE Type>
void MovePicker::score()
{
	Color c = pos.side_to_move();

	for (auto& m : *this)
	{
		if (Type == CAPTURES)
		{
			// Position::see()を用いると遅い。単に取る駒の価値順に調べたほうがパフォーマンス的にもいい。
			// 歩が成る指し手もあるのでこれはある程度優先されないといけない。
			// CAPTURE系である以上、打つ指し手は除外されている。

			// CAPTURES_PRO_PLUSで生成しているので歩の成る指し手が混じる。これは金と歩の価値の差の点数とする。

			// 移動させる駒の駒種。駒取りなので移動元は盤上であることは保証されている。
			auto pt = type_of(pos.piece_on(move_from(m)));
			// bool pawn_promo = is_promote(m) && pt == PAWN;

			// MVV-LVAに、歩の成りに加点する形にしておく。
			// →　歩の成りは加点しないほうがよさげ？
			m.value = // (pawn_promo ? (Value)(Eval::ProDiffPieceValue[PAWN]) : VALUE_ZERO) +
				(Value)Eval::CapturePieceValue[pos.piece_on(to_sq(m))]
				- LVA(pt);

			// 盤の上のほうの段にあるほど価値があるので下の方の段に対して小さなペナルティを課す。
			// (基本的には取る駒の価値が大きいほど優先であるから..)
			// m.value -= Value(1 * relative_rank(pos.side_to_move(), rank_of(move_to(m))));
			// →　将棋ではあまりよくないアイデア。

		}
		else if (Type == QUIETS)
		{
			// 駒を取らない指し手をオーダリングする。

			Piece movedPiece = pos.moved_piece_after(m);
			Square movedSq = to_sq(m);

			m.value = (*mainHistory)[from_to(m)][c]
					+ (*contHistory[0])[movedSq][movedPiece]
					+ (*contHistory[1])[movedSq][movedPiece]
					+ (*contHistory[3])[movedSq][movedPiece];
		}
		else // Type == EVASIONS
		{
			// 王手回避の指し手をスコアリングする。

			// 駒を取る指し手ならseeがプラスだったということなのでプラスの符号になるようにStats::Maxを足す。
			// あとは取る駒の価値を足して、動かす駒の番号を引いておく(小さな価値の駒で王手を回避したほうが
			// 価値が高いので(例えば合駒に安い駒を使う的な…)

			//  ・成るなら、その成りの価値を加算したほうが見積もりとしては正しい？
			// 　それは取り返されないことが前提にあるから、そうでもない。
			//		T1,r300,2491 - 78 - 2421(50.71% R4.95)
			//		T1,b1000,2483 - 103 - 2404(50.81% R5.62)
			//      T1,b3000,2459 - 148 - 2383(50.78% R5.45)
			//   →　やはり、改造前のほうが良い。[2016/10/06]

			// ・moved_piece_before()とmoved_piece_after()との比較
			// 　厳密なLVAではなくなるが、afterのほうが良さげ。
			// 　例えば、歩を成って取るのと、桂で取るのとでは、安い駒は歩だが、桂で行ったほうが、
			// 　歩はあとで成れるとすれば潜在的な価値はそちらのほうが高いから、そちらを残しておくという理屈はあるのか。
			//		T1, b1000, 2402 - 138 - 2460(49.4% R - 4.14) win black : white = 51.04% : 48.96%
			//		T1,b3000,1241 - 108 - 1231(50.2% R1.41) win black : white = 50.53% : 49.47%
			//		T1,b5000,1095 - 118 - 1047(51.12% R7.79) win black : white = 52.33% : 47.67%
			//  →　moved_piece_before()のほうで問題なさげ。[2017/5/20]

			if (pos.capture(m))
				// 捕獲する指し手に関しては簡易SEE + MVV/LVA
				m.value = (Value)Eval::CapturePieceValue[pos.piece_on(to_sq(m))]
				        - (Value)(LVA(type_of(pos.moved_piece_before(m)))) + Value(1 << 28);
			else
				// 捕獲しない指し手に関してはhistoryの値の順番
				m.value = (*mainHistory)[from_to(m)][c];

		}
	}
}


// 呼び出されるごとに新しいpseudo legalな指し手をひとつ返す。
// 指し手が尽きればMOVE_NONEが返る。
// 置換表の指し手(ttMove)を返したあとは、それを取り除いた指し手を返す。
Move MovePicker::next_move(bool skipQuiets) {

#ifdef MUST_CAPTURE_SHOGI_ENGINE
	// MustCaptureShogiの場合は、mustCaptureフラグを見ながら指し手を返す必要がある。
	Move move;
	while (true)
	{
		move = next_move2();

		// 終端まで行った
		if (move == MOVE_NONE)
			return move;

		// 1.mustCaputreモードではない
		// 2.mustCaptureだけどmoveが捕獲する指し手
		// のいずれか
		if (!mustCapture || pos.capture(move))
			return move;
	}
}
Move MovePicker::next_move2() {
#endif

	Move move;

	// 以下、caseのfall throughを駆使して書いてある。
	switch (stage)
	{
		// 置換表の指し手を返すフェーズ
	case MAIN_SEARCH: case EVASION: case QSEARCH_WITH_CHECKS:
	case QSEARCH_NO_CHECKS: case PROBCUT:
		++stage;
		return ttMove;

	case CAPTURES_INIT:
		endBadCaptures = cur = moves;
		endMoves = generateMoves<CAPTURES_PRO_PLUS>(pos, cur);
		score<CAPTURES>(); // CAPTUREの指し手の並べ替え。
		++stage;
		/* fallthrough */

		// 置換表の指し手を返したあとのフェーズ
		// (killer moveの前のフェーズなのでkiller除去は不要)
		// SSEの符号がマイナスのものはbad captureのほうに回す。
	case GOOD_CAPTURES:
		while (cur < endMoves)
		{
			move = pick_best(cur++, endMoves);
			if (move != ttMove)
			{
				// ここでSSEの符号がマイナスならbad captureのほうに回す。
				// ToDo: moveは駒打ちではないからsee()の内部での駒打ちは判定不要なのだが。
				if (pos.see_ge(move, VALUE_ZERO))
					return move;

				// 損をするCAPTUREの指し手は、後回しにする。
				*endBadCaptures++ = move;
			}
		}
		++stage;

		/* fallthrough */

	case KILLER0:
	case KILLER1:
		do
		{
		    move = killers[++stage - KILLER1];
		    if (    move != MOVE_NONE
		        &&  move != ttMove
		        &&  pos.pseudo_legal(move)
		        && !pos.capture(move))
		        return move;
		} while (stage <= KILLER1);
		/* fallthrough */

		// counter moveを返すフェーズ
	case COUNTERMOVE:
		++stage;
		move = countermove;
		if (   move != MOVE_NONE
			&& move != ttMove
			&& move != killers[0]
			&& move != killers[1]
			&& pos.pseudo_legal_s<false>(move)
			&& !pos.capture_or_pawn_promotion(move))
			return move;
		/* fallthrough */

	case QUIET_INIT:
		cur = endBadCaptures;
		endMoves = generateMoves<NON_CAPTURES_PRO_MINUS>(pos, cur);
		score<QUIETS>();

		// 指し手を部分的にソートする。depthに線形に依存する閾値で。
		partial_insertion_sort(cur, endMoves, -4000 * depth / ONE_PLY);

		++stage;
		/* fallthrough */

	// 捕獲しない指し手を返す。
	// (置換表の指し手とkillerの指し手は返したあとなのでこれらの指し手は除外する必要がある)
	// ※　これ、指し手の数が多い場合、AVXを使って一気に削除しておいたほうが良いのでは..
	case QUIET:
		if (!skipQuiets)
			while (cur < endMoves)
			{
				move = *cur++;
				if (move != ttMove
					&& move != killers[0]
					&& move != killers[1]
					&& move != countermove)
					return move;
			}
		++stage;

		// bad capturesの先頭を指すようにする。これは指し手生成バッファの先頭付近を再利用している。
		cur = moves;
		/* fallthrough */

		// see()が負の指し手を返す。
	case BAD_CAPTURES:
		if (cur < endBadCaptures)
			return *cur++;
		break;
		// ここでcaseのfall throughは終わり。

	// 回避手の生成
	case EVASIONS_INIT:
		cur = moves;
		endMoves = generateMoves<EVASIONS>(pos, cur);
		score<EVASIONS>();
		++stage;
		/* fallthrough */

	// 王手回避の指し手を返す
	case ALL_EVASIONS:
		while (cur < endMoves)
		{
			move = pick_best(cur++, endMoves);
			if (move != ttMove)
				return move;
		}
		break;

	case PROBCUT_INIT:
		cur = moves;
		endMoves = generateMoves<CAPTURES_PRO_PLUS>(pos, cur);
		score<CAPTURES>();
		++stage;
		/* fallthrough */

		// 通常探索のProbCutの処理から呼び出されるとき用。
		// 直前に捕獲された駒の価値以上のcaptureの指し手のみを生成する。
	case PROBCUT_CAPTURES:
		while (cur < endMoves)
		{
			move = pick_best(cur++, endMoves);
			if (move != ttMove
				&& pos.see_ge(move, threshold))
				return move;
		}
		break;

	// 捕獲する指し手のみを生成
	case QCAPTURES_1_INIT: case QCAPTURES_2_INIT:
		cur = moves;
		endMoves = generateMoves<CAPTURES_PRO_PLUS>(pos, cur);
		score<CAPTURES>();
		++stage;
		/* fallthrough */

	// 残りの指し手を生成するフェーズ(共通処理)
	case QCAPTURES_1: case QCAPTURES_2:
		while (cur < endMoves)
		{
			move = pick_best(cur++, endMoves);
			if (move != ttMove)
				return move;
		}
		if (stage == QCAPTURES_2)
			break;
		cur = moves;
		// CAPTURES_PRO_PLUSで生成していたので、歩の成る指し手は除外された成る指し手＋王手の指し手生成が必要。
		// QUIET_CHECKS_PRO_MINUSがあれば良いのだが、実装が難しいので、このあと除外する。
		endMoves = generateMoves<QUIET_CHECKS>(pos, cur);
		++stage;
		/* fallthrough */

		// 王手になる指し手を一手ずつ返すフェーズ
		// (置換表の指し手は返したあとなのでこの指し手は除外する必要がある)
	case QCHECKS:
		while (cur < endMoves)
		{
			move = cur++->move;
			if (move != ttMove
				&& !pos.pawn_promotion(move)
				)
				return move;
		}
		break;

	case QSEARCH_RECAPTURES:
		cur = moves;
		endMoves = generateMoves<RECAPTURES>(pos, moves, recaptureSquare);
		score<CAPTURES>(); // CAPTUREの指し手の並べ替え
		++stage;
		/* fallthrough */

	// 取り返す指し手。これはすでにrecaptureの指し手だけが生成されているのでそのまま返す。
	case QRECAPTURES:
		while (cur < endMoves)
		{
			// recaptureの指し手が2つ以上あることは稀なのでここでオーダリングしてもあまり意味をなさないが、
			// 生成される指し手自体が少ないなら、pick_best()のコストはほぼ無視できるのでこれはやらないよりはマシ。
			move = pick_best(cur++, endMoves);
			//if (to_sq(move) == recaptureSquare)
			//	return move;
			// →　recaptureの指し手のみを生成しているのでこの判定は不要。
			ASSERT_LV3(to_sq(move) == recaptureSquare);

			return move;
		}
		break;

	default:
		UNREACHABLE;
		return MOVE_NONE;
	}

	return MOVE_NONE;
}
