#include "imageviewerv2.h"

ImageViewerV2::ImageViewerV2(QWidget *parent) : QGraphicsView(parent),
    pixmap(nullptr),
    pixmapScaled(nullptr),
    movie(nullptr),
    transparencyGridEnabled(false),
    expandImage(false),
    smoothAnimatedImages(true),
    smoothUpscaling(true),
    forceFastScale(false),
    keepFitMode(false),
    loopPlayback(true),
    mIsFullscreen(false),
    absoluteStep(false),
    mouseInteraction(MouseInteractionState::MOUSE_NONE),
    minScale(0.01f),
    maxScale(500.0f),
    fitWindowScale(0.125f),
    mViewLock(LOCK_NONE),
    imageFitMode(FIT_WINDOW),
    mScalingFilter(QI_FILTER_BILINEAR),
    imageFitModeDefault(FIT_WINDOW),
    scene(nullptr)
{
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    this->viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);
    setAcceptDrops(false);

    dpr = this->devicePixelRatioF();

    scrollTimeLineY = new QTimeLine();
    scrollTimeLineY->setEasingCurve(QEasingCurve::OutSine);
    scrollTimeLineY->setDuration(ANIMATION_SPEED);
    scrollTimeLineY->setUpdateInterval(SCROLL_UPDATE_RATE);
    scrollTimeLineX = new QTimeLine();
    scrollTimeLineX->setEasingCurve(QEasingCurve::OutSine);
    scrollTimeLineX->setDuration(ANIMATION_SPEED);
    scrollTimeLineX->setUpdateInterval(SCROLL_UPDATE_RATE);
    connect(scrollTimeLineX, &QTimeLine::finished, this, &ImageViewerV2::onScrollTimelineFinished);
    connect(scrollTimeLineY, &QTimeLine::finished, this, &ImageViewerV2::onScrollTimelineFinished);

    animationTimer = new QTimer(this);
    animationTimer->setSingleShot(true);

    scaleTimer = new QTimer(this);
    scaleTimer->setSingleShot(true);
    scaleTimer->setInterval(80);

    lastTouchpadScroll.start();

    zoomThreshold = static_cast<int>(devicePixelRatioF() * 4.);

    pixmapItem.setTransformationMode(Qt::SmoothTransformation);
    pixmapItem.setScale(1.0f);
    pixmapItemScaled.setScale(1.0f);

    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    scene = new QGraphicsScene();
    scene->setSceneRect(0,0,200000,200000);
    scene->setBackgroundBrush(QColor(60,60,103));
    scene->addItem(&pixmapItem);
    scene->addItem(&pixmapItemScaled);
    pixmapItemScaled.hide();

    this->setFrameShape(QFrame::NoFrame);
    this->setScene(scene);

    connect(scrollTimeLineX, &QTimeLine::frameChanged, this, &ImageViewerV2::scrollToX);
    connect(scrollTimeLineY, &QTimeLine::frameChanged, this, &ImageViewerV2::scrollToY);

    connect(animationTimer, &QTimer::timeout, this, &ImageViewerV2::onAnimationTimer, Qt::UniqueConnection);

    QObject::connect(scaleTimer, &QTimer::timeout, [this]() {
        this->requestScaling();
    });

    readSettings();
    connect(settings, &Settings::settingsChanged, this, &ImageViewerV2::readSettings);
}

ImageViewerV2::~ImageViewerV2() {
}

void ImageViewerV2::readSettings() {
    smoothAnimatedImages = settings->smoothAnimatedImages();
    smoothUpscaling = settings->smoothUpscaling();
    expandImage = settings->expandImage();
    expandLimit = static_cast<float>(settings->expandLimit());
    if(expandLimit < 1.0f)
        expandLimit = maxScale;
    keepFitMode = settings->keepFitMode();
    imageFitModeDefault = settings->imageFitMode();
    zoomStep = settings->zoomStep();
    transparencyGridEnabled = settings->transparencyGrid();
    focusIn1to1 = settings->focusPointIn1to1Mode();
    absoluteStep = settings->absoluteZoomStep();
    // set bg color
    onFullscreenModeChanged(mIsFullscreen);
    updateMinScale();
    setScalingFilter(settings->scalingFilter());
    setFitMode(imageFitModeDefault);
}

void ImageViewerV2::onFullscreenModeChanged(bool mode) {
    QColor bgColor;
    mIsFullscreen = mode;
    if(mode) {
        bgColor = settings->colorScheme().background_fullscreen;
        bgColor.setAlphaF(1.0);
    } else {
        bgColor = settings->colorScheme().background;
        bgColor.setAlphaF(settings->backgroundOpacity());
    }
    scene->setBackgroundBrush(bgColor);
}

void ImageViewerV2::startAnimation() {
    if(movie && movie->frameCount() > 1) {
        stopAnimation();
        emit animationPaused(false);
        //movie->jumpToFrame(0);
        //emit frameChanged(0);
        animationTimer->start(movie->nextFrameDelay());
    }
}

void ImageViewerV2::stopAnimation() {
    if(movie) {
        emit animationPaused(true);
        animationTimer->stop();
    }
}

void ImageViewerV2::pauseResume() {
    if(movie) {
        if(animationTimer->isActive())
            stopAnimation();
        else
            startAnimation();
    }
}

void ImageViewerV2::onAnimationTimer() {
    if(!movie)
        return;
    if(movie->currentFrameNumber() == movie->frameCount() - 1) {
        // last frame
        if(!loopPlayback) {
            emit animationPaused(true);
            emit playbackFinished();
            return;
        } else {
            movie->jumpToFrame(0);
        }
    } else {
        if(!movie->jumpToNextFrame()) {
            qDebug() << "[Error] QMovie:" << movie->lastErrorString();
            this->stopAnimation();
            return;
        }
    }
    emit frameChanged(movie->currentFrameNumber());
    std::unique_ptr<QPixmap> newFrame(new QPixmap());
    *newFrame = movie->currentPixmap();
    updatePixmap(std::move(newFrame));
    animationTimer->start(movie->nextFrameDelay());
}

void ImageViewerV2::nextFrame() {
    if(!movie) {
        return;
    } else if(movie->currentFrameNumber() == movie->frameCount() - 1) {
        showAnimationFrame(0);
    } else {
        showAnimationFrame(movie->currentFrameNumber() + 1);
    }
}

void ImageViewerV2::prevFrame() {
    if(!movie) {
        return;
    } else if(movie->currentFrameNumber() == 0) {
        showAnimationFrame(movie->frameCount() - 1);
    } else {
        showAnimationFrame(movie->currentFrameNumber() - 1);
    }
}

bool ImageViewerV2::showAnimationFrame(int frame) {
    if(!movie || frame < 0 || frame >= movie->frameCount())
        return false;
    if(movie->currentFrameNumber() == frame)
        return true;
    // at the first glance this may seem retarded
    // because it is
    // unfortunately i dont see a *better* way to do seeking with QMovie
    // QMovie::CacheAll is buggy and memory inefficient
    if(frame < movie->currentFrameNumber())
        movie->jumpToFrame(0);
    while(frame != movie->currentFrameNumber()) {
        if(!movie->jumpToNextFrame()) {
            qDebug() << "[Error] QMovie:" << movie->lastErrorString();
            break;
        }
    }
    emit frameChanged(movie->currentFrameNumber());
    std::unique_ptr<QPixmap> newFrame(new QPixmap());
    *newFrame = movie->currentPixmap();
    updatePixmap(std::move(newFrame));
    return true;
}

void ImageViewerV2::updatePixmap(std::unique_ptr<QPixmap> newPixmap) {
    pixmap = std::move(newPixmap);
    pixmap->setDevicePixelRatio(dpr);
    pixmapItem.setPixmap(*pixmap);
    pixmapItem.show();
    // always scale from center
    pixmapItem.setOffset((scene->width()  / 2.0) - (pixmap->width()  / (dpr * 2.0)),
                         (scene->height() / 2.0) - (pixmap->height() / (dpr * 2.0)));
    // always scale from center
    pixmapItem.setTransformOriginPoint(pixmapItem.boundingRect().center());
    pixmapItem.update();
}

void ImageViewerV2::displayAnimation(std::shared_ptr<QMovie> _movie) {
    if(_movie && _movie->isValid()) {
        reset();
        movie = _movie;
        movie->jumpToFrame(0);
        Qt::TransformationMode mode = smoothAnimatedImages ? Qt::SmoothTransformation : Qt::FastTransformation;
        pixmapItem.setTransformationMode(mode);
        std::unique_ptr<QPixmap> newFrame(new QPixmap());
        *newFrame = movie->currentPixmap();
        updatePixmap(std::move(newFrame));
        emit durationChanged(movie->frameCount());
        emit frameChanged(0);

        updateMinScale();
        if(!keepFitMode || imageFitMode == FIT_FREE)
            imageFitMode = imageFitModeDefault;

        if(mViewLock == LOCK_NONE) {
            applyFitMode();
        } else {
            imageFitMode = FIT_FREE;
            fitFree(lockedScale);
            if(mViewLock == LOCK_ALL)
                applySavedViewportPos();
        }

        if(transparencyGridEnabled)
            drawTransparencyGrid();

        startAnimation();
    }
}

// display & initialize
void ImageViewerV2::displayImage(std::unique_ptr<QPixmap> _pixmap) {
    reset();
    if(_pixmap) {
        pixmapItemScaled.hide();
        pixmap = std::move(_pixmap);
        pixmap->setDevicePixelRatio(dpr);
        pixmapItem.setPixmap(*pixmap);
        Qt::TransformationMode mode = Qt::SmoothTransformation;
        if(mScalingFilter == QI_FILTER_NEAREST)
            mode = Qt::FastTransformation;
        pixmapItem.setTransformationMode(mode);
        pixmapItem.show();
        pixmapItem.setOffset((scene->width()  / 2.0) - (pixmap->width()  / (dpr * 2.0)),
                             (scene->height() / 2.0) - (pixmap->height() / (dpr * 2.0)));
        // always scale from center
        pixmapItem.setTransformOriginPoint(pixmapItem.boundingRect().center());
        updateMinScale();

        if(!keepFitMode || imageFitMode == FIT_FREE)
            imageFitMode = imageFitModeDefault;

        if(mViewLock == LOCK_NONE) {
            applyFitMode();
        } else {
            imageFitMode = FIT_FREE;
            fitFree(lockedScale);
            if(mViewLock == LOCK_ALL)
                applySavedViewportPos();
        }
        requestScaling();

        if(transparencyGridEnabled)
            drawTransparencyGrid();
        update();
    }
}

// reset state, remove image & stop animation
void ImageViewerV2::reset() {
    stopPosAnimation();
    pixmapItemScaled.setPixmap(QPixmap());
    pixmapScaled.reset(nullptr);
    pixmapItem.setPixmap(QPixmap());
    pixmapItem.setScale(1.0f);
    pixmap.reset();
    stopAnimation();
    movie = nullptr;
    centerOn(sceneRect().center());
    // when this view is not in focus this it won't update the background
    // so we force it here
    viewport()->update();
}

void ImageViewerV2::closeImage() {
    reset();
}

void ImageViewerV2::setScaledPixmap(std::unique_ptr<QPixmap> newFrame) {
    if(!movie && newFrame->size() != scaledSize() * dpr)
        return;

    pixmapScaled = std::move(newFrame);
    pixmapScaled->setDevicePixelRatio(dpr);
    pixmapItemScaled.setPixmap(*pixmapScaled);
    pixmapItemScaled.setOffset((scene->width()  / 2.0) - (pixmapScaled->width()  / (dpr * 2.0)),
                               (scene->height() / 2.0) - (pixmapScaled->height() / (dpr * 2.0)));
    pixmapItem.hide();
    pixmapItemScaled.show();
}

bool ImageViewerV2::isDisplaying() const {
    return (pixmap != nullptr);
}

void ImageViewerV2::scrollUp() {
    scroll(0, -SCROLL_DISTANCE, true);
}

void ImageViewerV2::scrollDown() {
    scroll(0, SCROLL_DISTANCE, true);
}

void ImageViewerV2::scrollLeft() {
    scroll(-SCROLL_DISTANCE, 0, true);
}

void ImageViewerV2::scrollRight() {
    scroll(SCROLL_DISTANCE, 0, true);
}

// temporary override till application restart
// todo - use a separate pixmap item?
void ImageViewerV2::toggleTransparencyGrid() {
    transparencyGridEnabled = !transparencyGridEnabled;
    // request a new one as the grid is baked into the current pixmap for performance reasons
    requestScaling();
}

void ImageViewerV2::setScalingFilter(ScalingFilter filter) {
    if(mScalingFilter == filter)
        return;
    mScalingFilter = filter;
    pixmapItem.setTransformationMode(selectTransformationMode());
    if(mScalingFilter == QI_FILTER_NEAREST)
        swapToOriginalPixmap();
    requestScaling();
}

void ImageViewerV2::setLoopPlayback(bool mode) {
    if(movie && mode && loopPlayback != mode)
        startAnimation();
    loopPlayback = mode;
}

void ImageViewerV2::setFilterNearest() {
    if(mScalingFilter != QI_FILTER_NEAREST) {
        mScalingFilter = QI_FILTER_NEAREST;
        pixmapItem.setTransformationMode(selectTransformationMode());
        swapToOriginalPixmap();
        requestScaling();
    }
}

void ImageViewerV2::setFilterBilinear() {
    if(mScalingFilter != QI_FILTER_BILINEAR) {
        mScalingFilter = QI_FILTER_BILINEAR;
        pixmapItem.setTransformationMode(selectTransformationMode());
        requestScaling();
    }
}

// returns a mode based on current zoom level and a bunch of toggles
Qt::TransformationMode ImageViewerV2::selectTransformationMode() {
    Qt::TransformationMode mode = Qt::SmoothTransformation;
    if(forceFastScale) {
        mode = Qt::FastTransformation;
    } else if(movie) {
        if(!smoothAnimatedImages || (pixmapItem.scale() > 1.0f && !smoothUpscaling))
            mode = Qt::FastTransformation;
    } else {
        if((pixmapItem.scale() > 1.0f && !smoothUpscaling) || mScalingFilter == QI_FILTER_NEAREST)
            mode = Qt::FastTransformation;
    }
    return mode;
}

void ImageViewerV2::setExpandImage(bool mode) {
    expandImage = mode;
    updateMinScale();
    applyFitMode();
    requestScaling();
}

void ImageViewerV2::show() {
    setMouseTracking(false);
    QGraphicsView::show();
    setMouseTracking(true);
}

void ImageViewerV2::hide() {
    setMouseTracking(false);
    QWidget::hide();
}

void ImageViewerV2::requestScaling() {
    if(!pixmap || pixmapItem.scale() == 1.0f || (!smoothUpscaling && pixmapItem.scale() >= 1.0f) || movie)
        return;
    // request "real" scaling when graphicsscene scaling is insufficient
    // (it uses a single pass bilinear which is sharp but produces artifacts on low zoom levels)
    if(currentScale() < FAST_SCALE_THRESHOLD)
        emit scalingRequested(scaledSize() * dpr, mScalingFilter);
}

void ImageViewerV2::drawTransparencyGrid() {
    // todo (via tiled pixmap? and just hide if disabled for zero overhead)
    /*if(pixmapScaled && pixmapScaled->hasAlphaChannel() && pixmapScaled->depth() != 8) {
        QPainter painter(pixmapScaled.get());
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        QColor dark(90,90,90,255);
        QColor light(140,140,140,255);
        int xCount, yCount;
        xCount = pixmapScaled->width() / CHECKBOARD_GRID_SIZE;
        yCount = pixmapScaled->height() / CHECKBOARD_GRID_SIZE;
        QRect square(0, 0, CHECKBOARD_GRID_SIZE, CHECKBOARD_GRID_SIZE);
        bool evenOdd;
        for(int i = 0; i <= yCount; i++) {
            evenOdd = (i % 2);
            for(int j = 0; j <= xCount; j++) {
                if(j % 2 == evenOdd)
                    painter.fillRect(square, light);
                square.translate(CHECKBOARD_GRID_SIZE, 0);
            }
            square.translate(0, CHECKBOARD_GRID_SIZE);
            square.moveLeft(0);
        }
        painter.fillRect(pixmapScaled->rect(), dark);
    }
    */
}

bool ImageViewerV2::imageFits() const {
    if(!pixmap)
        return true;
    return (pixmap->width()  <= (viewport()->width()  * devicePixelRatioF()) &&
            pixmap->height() <= (viewport()->height() * devicePixelRatioF()));
}

bool ImageViewerV2::scaledImageFits() const {
    if(!pixmap)
        return true;
    QSize sz = scaledSize();
    return (sz.width()  <= viewport()->width() &&
            sz.height() <= viewport()->height());
}

ScalingFilter ImageViewerV2::scalingFilter() const {
    return mScalingFilter;
}

QWidget *ImageViewerV2::widget() {
    return this;
}

bool ImageViewerV2::hasAnimation() const {
    return (movie != nullptr);
}

//  Right button zooming / dragging logic
//  mouseMoveStartPos: stores the previous mouseMoveEvent() position,
//                     used to calculate delta.
//  mousePressPos: used to filter out accidental zoom events
//  mouseInteraction: tracks which action we are performing since the last mousePressEvent()
//
void ImageViewerV2::mousePressEvent(QMouseEvent *event) {
    if(!pixmap) {
        QWidget::mousePressEvent(event);
        return;
    }
    mouseMoveStartPos = event->pos();
    mousePressPos = mouseMoveStartPos;
    if(event->button() & Qt::RightButton) {
        setZoomAnchor(event->pos());
    } else {
        QGraphicsView::mousePressEvent(event);
    }
}

void ImageViewerV2::mouseMoveEvent(QMouseEvent *event) {
    QWidget::mouseMoveEvent(event);
    if(!pixmap || mouseInteraction == MouseInteractionState::MOUSE_DRAG || mouseInteraction == MouseInteractionState::MOUSE_WHEEL_ZOOM)
        return;

    if(event->buttons() & Qt::LeftButton) {
        // ---------------- DRAG / PAN -------------------
        // select which action to start
        if(mouseInteraction == MouseInteractionState::MOUSE_NONE) {
            if(scaledImageFits()) {
                mouseInteraction = MouseInteractionState::MOUSE_DRAG_BEGIN;
            } else {
                mouseInteraction = MouseInteractionState::MOUSE_PAN;
                if(cursor().shape() != Qt::ClosedHandCursor)
                    setCursor(Qt::ClosedHandCursor);
            }
        }
        // emit a signal to start dnd; set flag to ignore further mouse move events
        if(mouseInteraction == MouseInteractionState::MOUSE_DRAG_BEGIN) {
            if( (abs(mousePressPos.x() - event->pos().x()) > dragThreshold) ||
                 abs(mousePressPos.y() - event->pos().y()) > dragThreshold)
            {
                mouseInteraction = MouseInteractionState::MOUSE_NONE;
                emit draggedOut();
            }
        }
        // panning
        if(mouseInteraction == MouseInteractionState::MOUSE_PAN) {
            mousePan(event);
        }
        return;
    } else if(event->buttons() & Qt::RightButton) {
        // ------------------- ZOOM ----------------------
        // filter out possible mouse jitter by ignoring low delta drags
        if(mouseInteraction == MouseInteractionState::MOUSE_ZOOM || abs(mousePressPos.y() - event->pos().y()) > zoomThreshold / dpr) {
            if(cursor().shape() != Qt::SizeVerCursor) {
                setCursor(Qt::SizeVerCursor);
            }
            mouseInteraction = MouseInteractionState::MOUSE_ZOOM;
            // avoid visible lags by forcing fast scale for large viewport sizes
            // this value possibly needs tweaking
            if(viewport()->width() * viewport()->height() > LARGE_VIEWPORT_SIZE)
                forceFastScale = true;
            mouseMoveZoom(event);
        }
        return;
    } else {
        event->ignore();
    }
}

void ImageViewerV2::mouseReleaseEvent(QMouseEvent *event) {
    unsetCursor();
    if(forceFastScale) {
        forceFastScale = false;
        pixmapItem.setTransformationMode(selectTransformationMode());
    }
    if(!pixmap || mouseInteraction == MouseInteractionState::MOUSE_NONE) {
        QGraphicsView::mouseReleaseEvent(event);
        event->ignore();
    }
    mouseInteraction = MouseInteractionState::MOUSE_NONE;
}

// warning for future me:
// for some reason in qgraphicsview wheelEvent is followed by moveEvent (wtf?)
void ImageViewerV2::wheelEvent(QWheelEvent *event) {
    #ifdef __APPLE__
    // we don't need these
    if(event->phase() == Qt::ScrollBegin || event->phase() == Qt::ScrollEnd) {
        event->accept();
        return;
    }
    #endif

    if(event->buttons() & Qt::RightButton) {
        event->accept();
        mouseInteraction = MOUSE_WHEEL_ZOOM;
        int angleDelta = event->angleDelta().ry();
        if(angleDelta > 0)
            zoomInCursor();
        else if(angleDelta < 0)
            zoomOutCursor();
    } else if(event->modifiers() == Qt::NoModifier) {
        event->accept();
        QPoint pixelDelta = event->pixelDelta();
        QPoint angleDelta = event->angleDelta();
        qDebug() << "pixelDelta:" << pixelDelta << "angleDelta:" << angleDelta;
        // high-precision touchpad
        if(pixelDelta != QPoint(0,0) && settings->imageScrolling() != ImageScrolling::SCROLL_NONE) {
            stopPosAnimation();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - pixelDelta.x() * TRACKPAD_SCROLL_MULTIPLIER);
            verticalScrollBar()->setValue(verticalScrollBar()->value() - pixelDelta.y() * TRACKPAD_SCROLL_MULTIPLIER);
            centerIfNecessary();
            snapToEdges();
        } else if(angleDelta != QPoint(0,0)) { // mouse wheel & (windows) touchpad
            // wheel usually sends angleDelta = 120 / 240 / ...
            // there doesnt seem to be a way to detect event source except this
            // this issue is windows only as both linux touchpad drivers send pixelDelta instead
            // as a workaround we use QElapsedTimer to guess where the event came from
            bool isWheel = angleDelta.y() && !(angleDelta.y() % 120);
            if(isWheel && lastTouchpadScroll.elapsed() > 100) {
                if(settings->imageScrolling() == SCROLL_BY_TRACKPAD_AND_WHEEL)
                    scroll(0, -angleDelta.y(), true);
                else
                    QWidget::wheelEvent(event);
                return; // return immediately so we wont restart the scroll timer
            } else if(settings->imageScrolling() != ImageScrolling::SCROLL_NONE) {
                stopPosAnimation();
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() - angleDelta.x() * TRACKPAD_SCROLL_MULTIPLIER);
                verticalScrollBar()->setValue(verticalScrollBar()->value() - angleDelta.y() * TRACKPAD_SCROLL_MULTIPLIER);
                centerIfNecessary();
                snapToEdges();
            }
            lastTouchpadScroll.restart();
        }
        saveViewportPos();
    } else {
        event->ignore();
        QWidget::wheelEvent(event);
    }
}

void ImageViewerV2::showEvent(QShowEvent *event) {
    QGraphicsView::showEvent(event);
    // ensure we are properly resized
    qApp->processEvents();
    // reapply fitmode to fix viewport position
    if(imageFitMode == FIT_ORIGINAL)
        applyFitMode();
}

// simple pan behavior (cursor stops at the screen edges)
inline
void ImageViewerV2::mousePan(QMouseEvent *event) {
    if(scaledImageFits())
        return;
    mouseMoveStartPos -= event->pos();
    scroll(mouseMoveStartPos.x(), mouseMoveStartPos.y(), false);
    mouseMoveStartPos = event->pos();
    saveViewportPos();
}

//  zooming while the right button is pressed
//  note: on reaching min zoom level the fitMode is set to FIT_WINDOW;
//        mid-zoom it is set to FIT_FREE.
//        FIT_FREE mode does not persist when changing images.
inline
void ImageViewerV2::mouseMoveZoom(QMouseEvent *event) {
    float stepMultiplier = 0.003f; // this one feels ok
    int currentPos = event->pos().y();
    int moveDistance = mouseMoveStartPos.y() - currentPos;
    float newScale = currentScale() * (1.0f + stepMultiplier * moveDistance * dpr);
    mouseMoveStartPos = event->pos();
    imageFitMode = FIT_FREE;

    zoomAnchored(newScale);
    centerIfNecessary();
    snapToEdges();
    if(pixmapItem.scale() == fitWindowScale)
        imageFitMode = FIT_WINDOW;
}

// scale at which current image fills the window
void ImageViewerV2::updateFitWindowScale() {
    float newMinScaleX = (float) viewport()->width()  * devicePixelRatioF() / pixmap->width();
    float newMinScaleY = (float) viewport()->height() * devicePixelRatioF() / pixmap->height();
    if(newMinScaleX < newMinScaleY) {
        fitWindowScale = newMinScaleX;
    } else {
        fitWindowScale = newMinScaleY;
    }
    if(expandImage && fitWindowScale > expandLimit)
        fitWindowScale = expandLimit;
}

// limit min scale to window size
void ImageViewerV2::updateMinScale() {
    if(!pixmap)
        return;
    updateFitWindowScale();
    if(imageFits())
        minScale = 1.0f;
    else
        minScale = fitWindowScale;
    if(mViewLock != LOCK_NONE && lockedScale < minScale)
        minScale = lockedScale;
}

void ImageViewerV2::fitWidth() {
    if(!pixmap)
        return;
    float scaleX = (float)viewport()->width() * devicePixelRatioF() / pixmap->width();
    if(!expandImage && scaleX > 1.0f)
        scaleX = 1.0f;
    if(scaleX > expandLimit)
        scaleX = expandLimit;
    if(currentScale() != scaleX) {
        swapToOriginalPixmap();
        doZoom(scaleX);
    }
    centerIfNecessary();
    // just center somewhere at the top then do snap
    if(scaledSize().height() > viewport()->height()) {
        QPointF centerTarget = mapToScene(viewport()->rect()).boundingRect().center();
        centerTarget.setY(0);
        centerOn(centerTarget);
    }
    snapToEdges();
}

void ImageViewerV2::fitWindow() {
    if(!pixmap)
        return;
    if(imageFits() && !expandImage) {
        fitNormal();
    } else {
        if(currentScale() != fitWindowScale) {
            swapToOriginalPixmap();
            doZoom(fitWindowScale);
        }
        centerOnPixmap();
    }
}

void ImageViewerV2::fitNormal() {
    fitFree(1.0f);
}

void ImageViewerV2::fitFree(float scale) {
    if(!pixmap)
        return;
    if(focusIn1to1 == FOCUS_TOP) {
        doZoom(scale);
        centerIfNecessary();
        if(scaledSize().height() > viewport()->height()) {
            QPointF centerTarget = sceneRect().center();
            centerTarget.setY(0);
            centerOn(centerTarget);
        }
        snapToEdges();
    } else {
        if(focusIn1to1 == FOCUS_CENTER)
            setZoomAnchor(viewport()->rect().center());
        else
            setZoomAnchor(mapFromGlobal(cursor().pos()));
        zoomAnchored(scale);
        centerIfNecessary();
        snapToEdges();
    }
}

void ImageViewerV2::applyFitMode() {
    switch(imageFitMode) {
        case FIT_ORIGINAL:
            fitNormal();
            break;
        case FIT_WIDTH:
            fitWidth();
            break;
        case FIT_WINDOW:
            fitWindow();
            break;
        default:
            break;
    }
}

// public, sends scale request
void ImageViewerV2::setFitMode(ImageFitMode newMode) {
    if(scaleTimer->isActive())
        scaleTimer->stop();
    stopPosAnimation();
    imageFitMode = newMode;
    applyFitMode();
    requestScaling();
}

// public, sends scale request
void ImageViewerV2::setFitOriginal() {
    setFitMode(FIT_ORIGINAL);
}

// public, sends scale request
void ImageViewerV2::setFitWidth() {
    setFitMode(FIT_WIDTH);
    requestScaling();
}

// public, sends scale request
void ImageViewerV2::setFitWindow() {
    setFitMode(FIT_WINDOW);
    requestScaling();
}

void ImageViewerV2::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    // reset this so we won't generate unnecessary drag'n'drop event
    mousePressPos = mapFromGlobal(cursor().pos());
    // Qt emits some unnecessary resizeEvents on startup
    // so we try to ignore them
    if(parentWidget()->isVisible()) {
        stopPosAnimation();
        updateMinScale();
        if(imageFitMode == FIT_FREE || imageFitMode == FIT_ORIGINAL) {
            centerIfNecessary();
            snapToEdges();
        } else {
            applyFitMode();
        }
        update();
        if(scaleTimer->isActive())
            scaleTimer->stop();
        scaleTimer->start();
        saveViewportPos();
    }
}

void ImageViewerV2::centerOnPixmap() {
    centerOn(pixmapItem.boundingRect().center());
}

void ImageViewerV2::stopPosAnimation() {
    if(scrollTimeLineX->state() == QTimeLine::Running)
        scrollTimeLineX->stop();
    if(scrollTimeLineY->state() == QTimeLine::Running)
        scrollTimeLineY->stop();
}

inline
void ImageViewerV2::scroll(int dx, int dy, bool smooth) {
    if(smooth) {
        scrollSmooth(dx, dy);
    } else {
        scrollPrecise(dx, dy);
    }
}

void ImageViewerV2::scrollSmooth(int dx, int dy) {
    if(dx) {
        int delta;
        if(dx < 0)
            delta = SCROLL_DISTANCE;
        else
            delta = -SCROLL_DISTANCE;
        bool redirect = false;
        int currentXPos = horizontalScrollBar()->value();
        int newEndFrame = currentXPos - static_cast<int>(delta);
        if( (newEndFrame < currentXPos && currentXPos < scrollTimeLineX->endFrame()) ||
            (newEndFrame > currentXPos && currentXPos > scrollTimeLineX->endFrame()) )
        {
            redirect = true;
        }
        if(scrollTimeLineX->state() == QTimeLine::Running) {
            int oldEndFrame = scrollTimeLineX->endFrame();
            //if(oldEndFrame == currentYPos)
            //    createScrollTimeLine();
            if(!redirect)
                newEndFrame = oldEndFrame - static_cast<int>(delta * SCROLL_SPEED_MILTIPLIER);
        }
        scrollTimeLineX->stop();
        scrollTimeLineX->setFrameRange(currentXPos, newEndFrame);
        scrollTimeLineX->start();
    }
    if(dy) {
        int delta;
        if(dy < 0)
            delta = SCROLL_DISTANCE;
        else
            delta = -SCROLL_DISTANCE;
        bool redirect = false;
        int currentYPos = verticalScrollBar()->value();
        int newEndFrame = currentYPos - static_cast<int>(delta);
        if( (newEndFrame < currentYPos && currentYPos < scrollTimeLineY->endFrame()) ||
            (newEndFrame > currentYPos && currentYPos > scrollTimeLineY->endFrame()) )
        {
            redirect = true;
        }
        if(scrollTimeLineY->state() == QTimeLine::Running) {
            int oldEndFrame = scrollTimeLineY->endFrame();
            //if(oldEndFrame == currentYPos)
            //    createScrollTimeLine();
            if(!redirect)
                newEndFrame = oldEndFrame - static_cast<int>(delta * SCROLL_SPEED_MILTIPLIER);
        }
        scrollTimeLineY->stop();
        scrollTimeLineY->setFrameRange(currentYPos, newEndFrame);
        scrollTimeLineY->start();
    }
    saveViewportPos();
}

void ImageViewerV2::scrollPrecise(int dx, int dy) {
    stopPosAnimation();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + dx);
    verticalScrollBar()->setValue(verticalScrollBar()->value() + dy);
    centerIfNecessary();
    snapToEdges();
    saveViewportPos();
}

// used by scrollTimeLine
void ImageViewerV2::scrollToX(int x) {
    horizontalScrollBar()->setValue(x);
    centerIfNecessary();
    snapToEdges();
    update();
    qApp->processEvents();
}

// used by scrollTimeLine
void ImageViewerV2::scrollToY(int y) {
    verticalScrollBar()->setValue(y);
    centerIfNecessary();
    snapToEdges();
    update();
    qApp->processEvents();
}

void ImageViewerV2::onScrollTimelineFinished() {
    saveViewportPos();
}

void ImageViewerV2::swapToOriginalPixmap() {
    if(!pixmap || !pixmapItemScaled.isVisible())
        return;
    pixmapItemScaled.hide();
    pixmapItemScaled.setPixmap(QPixmap());
    pixmapScaled.reset(nullptr);
    pixmapItem.show();
}

void ImageViewerV2::setZoomAnchor(QPoint viewportPos) {
    zoomAnchor = QPair<QPointF, QPoint>(pixmapItem.mapFromScene(mapToScene(viewportPos)),
                       viewportPos);
}

void ImageViewerV2::zoomAnchored(float newScale) {
    if(currentScale() != newScale) {
        QPointF vportCenter = mapToScene(viewport()->geometry()).boundingRect().center();
        doZoom(newScale);
        // calculate shift to adjust viewport center
        // we do this in viewport coordinates to avoid any rounding errors
        QPointF diff = zoomAnchor.second - mapFromScene(pixmapItem.mapToScene(zoomAnchor.first));
        centerOn(vportCenter - diff);
        requestScaling();
    }
}

// zoom in around viewport center
void ImageViewerV2::zoomIn() {
    setZoomAnchor(viewport()->rect().center());
    if(absoluteStep)
        zoomAnchored(currentScale() + zoomStep);
    else
        zoomAnchored(currentScale() * (1.0f + zoomStep));
    centerIfNecessary();
    snapToEdges();
    imageFitMode = FIT_FREE;
    if(pixmapItem.scale() == fitWindowScale)
        imageFitMode = FIT_WINDOW;
}

// zoom out around viewport center
void ImageViewerV2::zoomOut() {
    setZoomAnchor(viewport()->rect().center());
    if(absoluteStep)
        zoomAnchored(currentScale() - zoomStep);
    else
        zoomAnchored(currentScale() * (1.0f - zoomStep));
    centerIfNecessary();
    snapToEdges();
    imageFitMode = FIT_FREE;
    if(pixmapItem.scale() == fitWindowScale)
        imageFitMode = FIT_WINDOW;
}

void ImageViewerV2::toggleLockZoom() {
    if(!isDisplaying())
        return;
    if(mViewLock != LOCK_ZOOM) {
        mViewLock = LOCK_ZOOM;
        lockZoom();
    } else {
        mViewLock = LOCK_NONE;
    }
}

bool ImageViewerV2::lockZoomEnabled() {
    return (mViewLock == LOCK_ZOOM);
}

void ImageViewerV2::lockZoom() {
    lockedScale = pixmapItem.scale();
    imageFitMode = FIT_FREE;
    saveViewportPos();
}

void ImageViewerV2::toggleLockView() {
    if(!isDisplaying())
        return;
    if(mViewLock != LOCK_ALL) {
        mViewLock = LOCK_ALL;
        lockZoom();
        saveViewportPos();
    } else {
        mViewLock = LOCK_NONE;
    }
}

bool ImageViewerV2::lockViewEnabled() {
    return (mViewLock == LOCK_ALL);
}

// savedViewportPos is [0...1][0...1]
// values are where viewport center is on the image
void ImageViewerV2::saveViewportPos() {
    if(mViewLock != LOCK_ALL)
        return;
    QGraphicsPixmapItem *item = &pixmapItem;
    QPointF sceneCenter = mapToScene( viewport()->rect().center() ) + QPointF(1,1);
    auto itemRect = item->sceneBoundingRect();
    savedViewportPos.setX(qBound(qreal(0), (sceneCenter.x() - itemRect.left()) / itemRect.width(),  qreal(1)));
    savedViewportPos.setY(qBound(qreal(0), (sceneCenter.y() - itemRect.top())  / itemRect.height(), qreal(1)));
}

void ImageViewerV2::applySavedViewportPos() {
    QGraphicsPixmapItem *item = &pixmapItem;
    auto itemRect = item->sceneBoundingRect();
    QPointF newScenePos;
    newScenePos.setX(itemRect.left() + itemRect.width()  * savedViewportPos.x());
    newScenePos.setY(itemRect.top()  + itemRect.height() * savedViewportPos.y());
    centerOn(newScenePos);
    centerIfNecessary();
    snapToEdges();
}

void ImageViewerV2::centerIfNecessary() {
    if(!pixmap)
        return;
    QSize sz = scaledSize();
    QPointF centerTarget = mapToScene(viewport()->rect()).boundingRect().center();
    if(sz.width() <= viewport()->width())
        centerTarget.setX(sceneRect().center().x());
    if(sz.height() <= viewport()->height())
        centerTarget.setY(sceneRect().center().y());
    centerOn(centerTarget);
}

void ImageViewerV2::snapToEdges() {
    QRect imgRect = scaledRect();
    // current vport center
    QPointF centerTarget = mapToScene(viewport()->rect()).boundingRect().center();
    qreal xShift = 0;
    qreal yShift = 0;
    if(imgRect.width() > width()) {
        if(imgRect.left() > 0)
            xShift = imgRect.left();
        else if(imgRect.right() < width())
            xShift = imgRect.right() - width();
    }
    if(imgRect.height() > height()) {
        if(imgRect.top() > 0)
            yShift = imgRect.top();
        else if(imgRect.bottom() < height())
            yShift = imgRect.bottom() - height();
    }
    centerOn(centerTarget + QPointF(xShift, yShift));
}

// todo: just merge with zoomIn
void ImageViewerV2::zoomInCursor() {
    if(underMouse()) {
        setZoomAnchor(mapFromGlobal(cursor().pos()));
        if(absoluteStep)
            zoomAnchored(currentScale() + zoomStep);
        else
            zoomAnchored(currentScale() * (1.0f + zoomStep));
    } else {
        zoomIn();
    }
    imageFitMode = FIT_FREE;
    if(pixmapItem.scale() == fitWindowScale)
        imageFitMode = FIT_WINDOW;
    centerIfNecessary();
    snapToEdges();
}

void ImageViewerV2::zoomOutCursor() {
    if(underMouse()) {
        setZoomAnchor(mapFromGlobal(cursor().pos()));
        if(absoluteStep)
            zoomAnchored(currentScale() - zoomStep);
        else
            zoomAnchored(currentScale() * (1.0f - zoomStep));
    } else {
        zoomIn();
    }
    imageFitMode = FIT_FREE;
    if(pixmapItem.scale() == fitWindowScale)
        imageFitMode = FIT_WINDOW;
    centerIfNecessary();
    snapToEdges();
}

void ImageViewerV2::doZoom(float newScale) {
    if(!pixmap)
        return;
    newScale = qBound(minScale, newScale, 500.0f);
    pixmapItem.setScale(newScale);
    pixmapItem.setTransformationMode(selectTransformationMode());
    swapToOriginalPixmap();
    emit scaleChanged(newScale);
}

ImageFitMode ImageViewerV2::fitMode() const {
    return imageFitMode;
}

// rounds a point in scene coordinates so it stays on the same spot on viewport
QPointF ImageViewerV2::sceneRoundPos(QPointF scenePoint) const {
    return mapToScene(mapFromScene(scenePoint));
}

// rounds a rect in scene coordinates so it stays on the same spot on viewport
// the result is what's actually drawn on screen (incl. size)
QRectF ImageViewerV2::sceneRoundRect(QRectF sceneRect) const {
    QRectF rounded = QRectF(sceneRoundPos(sceneRect.topLeft()),
                            sceneRect.size());
    return QRectF(sceneRoundPos(sceneRect.topLeft()),
                  sceneRect.size());
}

// size as it appears on screen (rounded)
QSize ImageViewerV2::scaledSize() const {
    if(!pixmap)
        return QSize(0,0);
    QRectF pixmapSceneRect = pixmapItem.mapRectToScene(pixmapItem.boundingRect());
    return sceneRoundRect(pixmapSceneRect).size().toSize();
}

// in viewport coords
QRect ImageViewerV2::scaledRect() const {
    QRectF pixmapSceneRect = pixmapItem.mapRectToScene(pixmapItem.boundingRect());
    return QRect(mapFromScene(pixmapSceneRect.topLeft()),
                 mapFromScene(pixmapSceneRect.bottomRight()));
}

float ImageViewerV2::currentScale() const {
    return pixmapItem.scale();
}

QSize ImageViewerV2::sourceSize() const {
    if(!pixmap)
        return QSize(0,0);
    return pixmap->size();
}
