/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/media/history_media_poll.h"

#include "lang/lang_keys.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "calls/calls_instance.h"
#include "ui/text_options.h"
#include "ui/effects/animations.h"
#include "ui/effects/radial_animation.h"
#include "ui/effects/ripple_animation.h"
#include "data/data_media_types.h"
#include "data/data_poll.h"
#include "data/data_session.h"
#include "layout.h"
#include "auth_session.h"
#include "apiwrap.h"
#include "styles/style_history.h"
#include "styles/style_widgets.h"

namespace {

using TextState = HistoryView::TextState;

struct FormattedLargeNumber {
	int rounded = 0;
	bool shortened = false;
	QString text;
};

FormattedLargeNumber FormatLargeNumber(int64 number) {
	auto result = FormattedLargeNumber();
	const auto abs = std::abs(number);
	const auto shorten = [&](int64 divider, char multiplier) {
		const auto sign = (number > 0) ? 1 : -1;
		const auto rounded = abs / (divider / 10);
		result.rounded = sign * rounded * (divider / 10);
		result.text = QString::number(sign * rounded / 10);
		if (rounded % 10) {
			result.text += '.' + QString::number(rounded % 10) + multiplier;
		} else {
			result.text += multiplier;
		}
		result.shortened = true;
	};
	if (abs >= 1'000'000) {
		shorten(1'000'000, 'M');
	} else if (abs >= 10'000) {
		shorten(1'000, 'K');
	} else {
		result.rounded = number;
		result.text = QString::number(number);
	}
	return result;
}

struct PercentCounterItem {
	int index = 0;
	int percent = 0;
	int remainder = 0;

	inline bool operator<(const PercentCounterItem &other) const {
		if (remainder > other.remainder) {
			return true;
		} else if (remainder < other.remainder) {
			return false;
		}
		return percent < other.percent;
	}
};

void AdjustPercentCount(gsl::span<PercentCounterItem> items, int left) {
	ranges::sort(items, std::less<>());
	for (auto i = 0, count = int(items.size()); i != count;) {
		const auto &item = items[i];
		auto j = i + 1;
		for (; j != count; ++j) {
			if (items[j].percent != item.percent
				|| items[j].remainder != item.remainder) {
				break;
			}
		}
		const auto equal = j - i;
		if (equal <= left) {
			left -= equal;
			for (; i != j; ++i) {
				++items[i].percent;
			}
		} else {
			i = j;
		}
	}
}

void CountNicePercent(
		gsl::span<const int> votes,
		int total,
		gsl::span<int> result) {
	Expects(result.size() >= votes.size());
	Expects(votes.size() <= PollData::kMaxOptions);

	const auto count = size_type(votes.size());
	PercentCounterItem ItemsStorage[PollData::kMaxOptions];
	const auto items = gsl::make_span(ItemsStorage).subspan(0, count);
	auto left = 100;
	auto &&zipped = ranges::view::zip(
		votes,
		items,
		ranges::view::ints(0));
	for (auto &&[votes, item, index] : zipped) {
		item.index = index;
		item.percent = (votes * 100) / total;
		item.remainder = (votes * 100) - (item.percent * total);
		left -= item.percent;
	}
	if (left > 0 && left <= count) {
		AdjustPercentCount(items, left);
	}
	for (const auto &item : items) {
		result[item.index] = item.percent;
	}
}

} // namespace

struct HistoryPoll::AnswerAnimation {
	anim::value percent;
	anim::value filling;
	anim::value opacity;
};

struct HistoryPoll::AnswersAnimation {
	std::vector<AnswerAnimation> data;
	Ui::Animations::Simple progress;
};

struct HistoryPoll::SendingAnimation {
	template <typename Callback>
	SendingAnimation(
		const QByteArray &option,
		Callback &&callback);

	QByteArray option;
	Ui::InfiniteRadialAnimation animation;
};

struct HistoryPoll::Answer {
	Answer();

	void fillText(const PollAnswer &original);

	Text text;
	QByteArray option;
	int votes = 0;
	int votesPercent = 0;
	int votesPercentWidth = 0;
	float64 filling = 0.;
	QString votesPercentString;
	bool chosen = false;
	ClickHandlerPtr handler;
	mutable std::unique_ptr<Ui::RippleAnimation> ripple;
};

template <typename Callback>
HistoryPoll::SendingAnimation::SendingAnimation(
	const QByteArray &option,
	Callback &&callback)
: option(option)
, animation(
	std::forward<Callback>(callback),
	st::historyPollRadialAnimation) {
}

HistoryPoll::Answer::Answer() : text(st::msgMinWidth / 2) {
}

void HistoryPoll::Answer::fillText(const PollAnswer &original) {
	if (!text.isEmpty() && text.toString() == original.text) {
		return;
	}
	text.setText(
		st::historyPollAnswerStyle,
		original.text,
		Ui::WebpageTextTitleOptions());
}

HistoryPoll::HistoryPoll(
	not_null<Element*> parent,
	not_null<PollData*> poll)
: HistoryMedia(parent)
, _poll(poll)
, _question(st::msgMinWidth / 2) {
	history()->owner().registerPollView(_poll, _parent);
}

QSize HistoryPoll::countOptimalSize() {
	updateTexts();

	const auto paddings = st::msgPadding.left() + st::msgPadding.right();

	auto maxWidth = st::msgFileMinWidth;
	accumulate_max(maxWidth, paddings + _question.maxWidth());
	for (const auto &answer : _answers) {
		accumulate_max(
			maxWidth,
			paddings
			+ st::historyPollAnswerPadding.left()
			+ answer.text.maxWidth()
			+ st::historyPollAnswerPadding.right());
	}

	const auto answersHeight = ranges::accumulate(ranges::view::all(
		_answers
	) | ranges::view::transform([](const Answer &answer) {
		return st::historyPollAnswerPadding.top()
			+ answer.text.minHeight()
			+ st::historyPollAnswerPadding.bottom();
	}), 0);

	auto minHeight = st::historyPollQuestionTop
		+ _question.minHeight()
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip
		+ answersHeight
		+ st::msgPadding.bottom()
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
	if (!isBubbleTop()) {
		minHeight -= st::msgFileTopMinus;
	}
	return { maxWidth, minHeight };
}

bool HistoryPoll::canVote() const {
	return !_voted && !_closed;
}

int HistoryPoll::countAnswerTop(
		const Answer &answer,
		int innerWidth) const {
	auto tshift = st::historyPollQuestionTop;
	if (!isBubbleTop()) {
		tshift -= st::msgFileTopMinus;
	}
	tshift += _question.countHeight(innerWidth) + st::historyPollSubtitleSkip;
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;
	auto &&answers = ranges::view::zip(
		_answers,
		ranges::view::ints(0, int(_answers.size())));
	const auto i = ranges::find(
		_answers,
		&answer,
		[](const Answer &answer) { return &answer; });
	const auto countHeight = [&](const Answer &answer) {
		return countAnswerHeight(answer, innerWidth);
	};
	tshift += ranges::accumulate(
		begin(_answers),
		i,
		0,
		ranges::plus(),
		countHeight);
	return tshift;
}

int HistoryPoll::countAnswerHeight(
		const Answer &answer,
		int innerWidth) const {
	const auto answerWidth = innerWidth
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();
	return st::historyPollAnswerPadding.top()
		+ answer.text.countHeight(answerWidth)
		+ st::historyPollAnswerPadding.bottom();
}

QSize HistoryPoll::countCurrentSize(int newWidth) {
	const auto paddings = st::msgPadding.left() + st::msgPadding.right();

	accumulate_min(newWidth, maxWidth());
	const auto innerWidth = newWidth
		- st::msgPadding.left()
		- st::msgPadding.right();

	const auto answersHeight = ranges::accumulate(ranges::view::all(
		_answers
	) | ranges::view::transform([&](const Answer &answer) {
		return countAnswerHeight(answer, innerWidth);
	}), 0);

	auto newHeight = st::historyPollQuestionTop
		+ _question.countHeight(innerWidth)
		+ st::historyPollSubtitleSkip
		+ st::msgDateFont->height
		+ st::historyPollAnswersSkip
		+ answersHeight
		+ st::historyPollTotalVotesSkip
		+ st::msgDateFont->height
		+ st::msgPadding.bottom();
	if (!isBubbleTop()) {
		newHeight -= st::msgFileTopMinus;
	}
	return { newWidth, newHeight };
}

void HistoryPoll::updateTexts() {
	if (_pollVersion == _poll->version) {
		return;
	}
	_pollVersion = _poll->version;

	const auto willStartAnimation = checkAnimationStart();

	if (_question.toString() != _poll->question) {
		_question.setText(
			st::historyPollQuestionStyle,
			_poll->question,
			Ui::WebpageTextTitleOptions());
	}
	if (_closed != _poll->closed || _subtitle.isEmpty()) {
		_closed = _poll->closed;
		_subtitle.setText(
			st::msgDateTextStyle,
			lang(_closed ? lng_polls_closed : lng_polls_anonymous));
	}

	updateAnswers();
	updateVotes();

	if (willStartAnimation) {
		startAnswersAnimation();
	}
}

void HistoryPoll::updateAnswers() {
	const auto changed = !ranges::equal(
		_answers,
		_poll->answers,
		ranges::equal_to(),
		&Answer::option,
		&PollAnswer::option);
	if (!changed) {
		auto &&answers = ranges::view::zip(_answers, _poll->answers);
		for (auto &&[answer, original] : answers) {
			answer.fillText(original);
		}
		return;
	}
	_answers = ranges::view::all(
		_poll->answers
	) | ranges::view::transform([](const PollAnswer &answer) {
		auto result = Answer();
		result.option = answer.option;
		result.fillText(answer);
		return result;
	}) | ranges::to_vector;

	for (auto &answer : _answers) {
		answer.handler = createAnswerClickHandler(answer);
	}

	resetAnswersAnimation();
}

ClickHandlerPtr HistoryPoll::createAnswerClickHandler(
		const Answer &answer) const {
	const auto option = answer.option;
	const auto itemId = _parent->data()->fullId();
	return std::make_shared<LambdaClickHandler>([=] {
		history()->session().api().sendPollVotes(itemId, { option });
	});
}

void HistoryPoll::updateVotes() {
	_voted = _poll->voted();
	updateAnswerVotes();
	updateTotalVotes();
}

void HistoryPoll::checkSendingAnimation() const {
	const auto &sending = _poll->sendingVote;
	if (sending.isEmpty() == !_sendingAnimation) {
		if (_sendingAnimation) {
			_sendingAnimation->option = sending;
		}
		return;
	}
	if (sending.isEmpty()) {
		if (!_answersAnimation) {
			_sendingAnimation = nullptr;
		}
		return;
	}
	_sendingAnimation = std::make_unique<SendingAnimation>(
		sending,
		[=] { radialAnimationCallback(); });
	_sendingAnimation->animation.start();
}

void HistoryPoll::updateTotalVotes() {
	if (_totalVotes == _poll->totalVoters && !_totalVotesLabel.isEmpty()) {
		return;
	}
	_totalVotes = _poll->totalVoters;
	const auto string = [&] {
		if (!_totalVotes) {
			return lang(lng_polls_votes_none);
		}
		const auto format = FormatLargeNumber(_totalVotes);
		auto text = lng_polls_votes_count(lt_count, format.rounded);
		if (format.shortened) {
			text.replace(QString::number(format.rounded), format.text);
		}
		return text;
	}();
	_totalVotesLabel.setText(st::msgDateTextStyle, string);
}

void HistoryPoll::updateAnswerVotesFromOriginal(
		Answer &answer,
		const PollAnswer &original,
		int percent,
		int maxVotes) {
	if (canVote()) {
		answer.votesPercent = 0;
		answer.votesPercentString.clear();
		answer.votesPercentWidth = 0;
	} else if (answer.votesPercentString.isEmpty()
		|| answer.votesPercent != percent) {
		answer.votesPercent = percent;
		answer.votesPercentString = QString::number(percent) + '%';
		answer.votesPercentWidth = st::historyPollPercentFont->width(
			answer.votesPercentString);
	}
	answer.votes = original.votes;
	answer.filling = answer.votes / float64(maxVotes);
}

void HistoryPoll::updateAnswerVotes() {
	if (_poll->answers.size() != _answers.size()
		|| _poll->answers.empty()) {
		return;
	}
	const auto totalVotes = std::max(1, _poll->totalVoters);
	const auto maxVotes = std::max(1, ranges::max_element(
		_poll->answers,
		ranges::less(),
		&PollAnswer::votes)->votes);

	constexpr auto kMaxCount = PollData::kMaxOptions;
	const auto count = size_type(_poll->answers.size());
	Assert(count <= kMaxCount);
	int PercentsStorage[kMaxCount] = { 0 };
	int VotesStorage[kMaxCount] = { 0 };

	ranges::copy(
		ranges::view::all(
			_poll->answers
		) | ranges::view::transform(&PollAnswer::votes),
		ranges::begin(VotesStorage));

	CountNicePercent(
		gsl::make_span(VotesStorage).subspan(0, count),
		totalVotes,
		gsl::make_span(PercentsStorage).subspan(0, count));

	auto &&answers = ranges::view::zip(
		_answers,
		_poll->answers,
		PercentsStorage);
	for (auto &&[answer, original, percent] : answers) {
		updateAnswerVotesFromOriginal(
			answer,
			original,
			percent,
			maxVotes);
	}
}

void HistoryPoll::draw(Painter &p, const QRect &r, TextSelection selection, crl::time ms) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) return;
	auto paintx = 0, painty = 0, paintw = width(), painth = height();

	checkSendingAnimation();
	_poll->checkResultsReload(_parent->data(), ms);

	const auto outbg = _parent->hasOutLayout();
	const auto selected = (selection == FullSelection);
	const auto &regular = selected ? (outbg ? st::msgOutDateFgSelected : st::msgInDateFgSelected) : (outbg ? st::msgOutDateFg : st::msgInDateFg);

	const auto padding = st::msgPadding;
	auto tshift = st::historyPollQuestionTop;
	if (!isBubbleTop()) {
		tshift -= st::msgFileTopMinus;
	}
	paintw -= padding.left() + padding.right();

	p.setPen(outbg ? st::webPageTitleOutFg : st::webPageTitleInFg);
	_question.drawLeft(p, padding.left(), tshift, paintw, width(), style::al_left, 0, -1, selection);
	tshift += _question.countHeight(paintw) + st::historyPollSubtitleSkip;

	p.setPen(regular);
	_subtitle.drawLeftElided(p, padding.left(), tshift, paintw, width());
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;

	const auto progress = _answersAnimation
		? _answersAnimation->progress.value(1.)
		: 1.;
	if (progress == 1.) {
		resetAnswersAnimation();
	}

	auto &&answers = ranges::view::zip(
		_answers,
		ranges::view::ints(0, int(_answers.size())));
	for (const auto &[answer, index] : answers) {
		const auto animation = _answersAnimation
			? &_answersAnimation->data[index]
			: nullptr;
		if (animation) {
			animation->percent.update(progress, anim::linear);
			animation->filling.update(progress, anim::linear);
			animation->opacity.update(progress, anim::linear);
		}
		const auto height = paintAnswer(
			p,
			answer,
			animation,
			padding.left(),
			tshift,
			paintw,
			width(),
			selection);
		tshift += height;
	}
	if (!_totalVotesLabel.isEmpty()) {
		tshift += st::msgPadding.bottom();
		p.setPen(regular);
		_totalVotesLabel.drawLeftElided(
			p,
			padding.left(),
			tshift,
			std::min(
				_totalVotesLabel.maxWidth(),
				paintw - _parent->infoWidth()),
			width());
	}
}

void HistoryPoll::resetAnswersAnimation() const {
	_answersAnimation = nullptr;
	if (_poll->sendingVote.isEmpty()) {
		_sendingAnimation = nullptr;
	}
}

void HistoryPoll::radialAnimationCallback() const {
	if (!anim::Disabled()) {
		history()->owner().requestViewRepaint(_parent);
	}
}

int HistoryPoll::paintAnswer(
		Painter &p,
		const Answer &answer,
		const AnswerAnimation *animation,
		int left,
		int top,
		int width,
		int outerWidth,
		TextSelection selection) const {
	const auto height = countAnswerHeight(answer, width);
	const auto outbg = _parent->hasOutLayout();
	const auto aleft = left + st::historyPollAnswerPadding.left();
	const auto awidth = width
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();

	if (answer.ripple) {
		p.setOpacity(st::historyPollRippleOpacity);
		answer.ripple->paint(p, left - st::msgPadding.left(), top, outerWidth);
		if (answer.ripple->empty()) {
			answer.ripple.reset();
		}
		p.setOpacity(1.);
	}

	if (animation) {
		const auto opacity = animation->opacity.current();
		if (opacity < 1.) {
			p.setOpacity(1. - opacity);
			paintRadio(p, answer, left, top, selection);
		}
		if (opacity > 0.) {
			const auto percent = QString::number(
				int(std::round(animation->percent.current()))) + '%';
			const auto percentWidth = st::historyPollPercentFont->width(
				percent);
			p.setOpacity(opacity);
			paintPercent(
				p,
				percent,
				percentWidth,
				left,
				top,
				outerWidth,
				selection);
			p.setOpacity(sqrt(opacity));
			paintFilling(
				p,
				animation->filling.current(),
				left,
				top,
				width,
				height,
				selection);
			p.setOpacity(1.);
		}
	} else if (canVote()) {
		paintRadio(p, answer, left, top, selection);
	} else {
		paintPercent(
			p,
			answer.votesPercentString,
			answer.votesPercentWidth,
			left,
			top,
			outerWidth,
			selection);
		paintFilling(
			p,
			answer.filling,
			left,
			top,
			width,
			height,
			selection);
	}

	top += st::historyPollAnswerPadding.top();
	p.setPen(outbg ? st::webPageDescriptionOutFg : st::webPageDescriptionInFg);
	answer.text.drawLeft(p, aleft, top, awidth, outerWidth);

	return height;
}

void HistoryPoll::paintRadio(
		Painter &p,
		const Answer &answer,
		int left,
		int top,
		TextSelection selection) const {
	top += st::historyPollAnswerPadding.top();

	const auto outbg = _parent->hasOutLayout();
	const auto selected = (selection == FullSelection);

	PainterHighQualityEnabler hq(p);
	const auto &st = st::historyPollRadio;
	const auto over = ClickHandler::showAsActive(answer.handler);
	const auto &regular = selected ? (outbg ? st::msgOutDateFgSelected : st::msgInDateFgSelected) : (outbg ? st::msgOutDateFg : st::msgInDateFg);

	p.setBrush(Qt::NoBrush);
	const auto o = p.opacity();
	p.setOpacity(o * (over ? st::historyPollRadioOpacityOver : st::historyPollRadioOpacity));

	const auto rect = QRectF(left, top, st.diameter, st.diameter).marginsRemoved(QMarginsF(st.thickness / 2., st.thickness / 2., st.thickness / 2., st.thickness / 2.));
	if (_sendingAnimation && _sendingAnimation->option == answer.option) {
		const auto &active = selected ? (outbg ? st::msgOutServiceFgSelected : st::msgInServiceFgSelected) : (outbg ? st::msgOutServiceFg : st::msgInServiceFg);
		if (anim::Disabled()) {
			anim::DrawStaticLoading(p, rect, st.thickness, active);
		} else {
			const auto state = _sendingAnimation->animation.computeState();
			auto pen = anim::pen(regular, active, state.shown);
			pen.setWidth(st.thickness);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.drawArc(
				rect,
				state.arcFrom,
				state.arcLength);
		}
	} else {
		auto pen = regular->p;
		pen.setWidth(st.thickness);
		p.setPen(pen);
		p.drawEllipse(rect);
	}

	p.setOpacity(o);
}

void HistoryPoll::paintPercent(
		Painter &p,
		const QString &percent,
		int percentWidth,
		int left,
		int top,
		int outerWidth,
		TextSelection selection) const {
	const auto outbg = _parent->hasOutLayout();
	const auto selected = (selection == FullSelection);
	const auto aleft = left + st::historyPollAnswerPadding.left();

	top += st::historyPollAnswerPadding.top();

	p.setFont(st::historyPollPercentFont);
	p.setPen(outbg ? st::webPageDescriptionOutFg : st::webPageDescriptionInFg);
	const auto pleft = aleft - percentWidth - st::historyPollPercentSkip;
	p.drawTextLeft(pleft, top + st::historyPollPercentTop, outerWidth, percent, percentWidth);
}

void HistoryPoll::paintFilling(
		Painter &p,
		float64 filling,
		int left,
		int top,
		int width,
		int height,
		TextSelection selection) const {
	const auto bottom = top + height;
	const auto outbg = _parent->hasOutLayout();
	const auto selected = (selection == FullSelection);
	const auto aleft = left + st::historyPollAnswerPadding.left();
	const auto awidth = width
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();

	top += st::historyPollAnswerPadding.top();

	const auto bar = outbg ? (selected ? st::msgWaveformOutActiveSelected : st::msgWaveformOutActive) : (selected ? st::msgWaveformInActiveSelected : st::msgWaveformInActive);
	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	p.setBrush(bar);
	const auto max = awidth - st::historyPollFillingRight;
	const auto size = anim::interpolate(st::historyPollFillingMin, max, filling);
	const auto radius = st::historyPollFillingRadius;
	const auto ftop = bottom - st::historyPollFillingBottom - st::historyPollFillingHeight;
	p.drawRoundedRect(aleft, ftop, size, st::historyPollFillingHeight, radius, radius);
}

bool HistoryPoll::answerVotesChanged() const {
	if (_poll->answers.size() != _answers.size()
		|| _poll->answers.empty()) {
		return false;
	}
	return !ranges::equal(
		_answers,
		_poll->answers,
		ranges::equal_to(),
		&Answer::votes,
		&PollAnswer::votes);
}

void HistoryPoll::saveStateInAnimation() const {
	if (_answersAnimation) {
		return;
	}
	const auto can = canVote();
	_answersAnimation = std::make_unique<AnswersAnimation>();
	_answersAnimation->data.reserve(_answers.size());
	const auto convert = [&](const Answer &answer) {
		auto result = AnswerAnimation();
		result.percent = can ? 0. : float64(answer.votesPercent);
		result.filling = can ? 0. : answer.filling;
		result.opacity = can ? 0. : 1.;
		return result;
	};
	ranges::transform(
		_answers,
		ranges::back_inserter(_answersAnimation->data),
		convert);
}

bool HistoryPoll::checkAnimationStart() const {
	if (_poll->answers.size() != _answers.size()) {
		// Skip initial changes.
		return false;
	}
	const auto result = (canVote() != (!_poll->voted() && !_poll->closed))
		|| answerVotesChanged();
	if (result) {
		saveStateInAnimation();
	}
	return result;
}

void HistoryPoll::startAnswersAnimation() const {
	if (!_answersAnimation) {
		return;
	}

	const auto can = canVote();
	auto &&both = ranges::view::zip(_answers, _answersAnimation->data);
	for (auto &&[answer, data] : both) {
		data.percent.start(can ? 0. : float64(answer.votesPercent));
		data.filling.start(can ? 0. : answer.filling);
		data.opacity.start(can ? 0. : 1.);
	}
	_answersAnimation->progress.start(
		[=] { history()->owner().requestViewRepaint(_parent); },
		0.,
		1.,
		st::historyPollDuration);
}

TextState HistoryPoll::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	if (!_poll->sendingVote.isEmpty()) {
		return result;
	}

	const auto can = canVote();
	const auto padding = st::msgPadding;
	auto paintw = width();
	auto tshift = st::historyPollQuestionTop;
	if (!isBubbleTop()) {
		tshift -= st::msgFileTopMinus;
	}
	paintw -= padding.left() + padding.right();

	tshift += _question.countHeight(paintw) + st::historyPollSubtitleSkip;
	tshift += st::msgDateFont->height + st::historyPollAnswersSkip;
	const auto awidth = paintw
		- st::historyPollAnswerPadding.left()
		- st::historyPollAnswerPadding.right();
	for (const auto &answer : _answers) {
		const auto height = countAnswerHeight(answer, paintw);
		if (point.y() >= tshift && point.y() < tshift + height) {
			if (can) {
				_lastLinkPoint = point;
				result.link = answer.handler;
			} else {
				result.customTooltip = true;
				using Flag = Text::StateRequest::Flag;
				if (request.flags & Flag::LookupCustomTooltip) {
					result.customTooltipText = answer.votes
						? lng_polls_votes_count(lt_count, answer.votes)
						: lang(lng_polls_votes_none);
				}
			}
			return result;
		}
		tshift += height;
	}
	return result;
}

void HistoryPoll::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	if (!handler) return;

	const auto i = ranges::find(
		_answers,
		handler,
		&Answer::handler);
	if (i != end(_answers)) {
		toggleRipple(*i, pressed);
	}
}

void HistoryPoll::toggleRipple(Answer &answer, bool pressed) {
	if (pressed) {
		const auto outerWidth = width();
		const auto innerWidth = outerWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		if (!answer.ripple) {
			auto mask = Ui::RippleAnimation::rectMask(QSize(
				outerWidth,
				countAnswerHeight(answer, innerWidth)));
			answer.ripple = std::make_unique<Ui::RippleAnimation>(
				(_parent->hasOutLayout()
					? st::historyPollRippleOut
					: st::historyPollRippleIn),
				std::move(mask),
				[=] { history()->owner().requestViewRepaint(_parent); });
		}
		const auto top = countAnswerTop(answer, innerWidth);
		answer.ripple->add(_lastLinkPoint - QPoint(0, top));
	} else {
		if (answer.ripple) {
			answer.ripple->lastStop();
		}
	}
}

HistoryPoll::~HistoryPoll() {
	history()->owner().unregisterPollView(_poll, _parent);
}
