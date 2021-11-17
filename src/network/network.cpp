//
// Created by dominik on 22. 10. 2021.
//

#include <chrono>
#include "network.h"
#include "../statistics/weights_info.h"

auto Network::forwardPass(const Matrix<ELEMENT_TYPE> &data, const std::vector<unsigned int> &labels) {
    activationResults.clear();
    activationDerivResults.clear();
    // Input layer has activation fn equal to identity.
    activationResults.push_back(data);

    auto tmp = data.matmul(weights[0]);
    tmp += biases[0];

    networkConfig.layersConfig[1].activationFunction(tmp);
    activationResults.push_back(tmp);

    if (weights.size() == 1) {
        activationDerivResults.emplace_back();
    }
    else {
        auto tmpCopy = tmp;
        networkConfig.layersConfig[1].activationDerivFunction(tmpCopy);
        activationDerivResults.push_back(tmpCopy);
    }

    for (size_t i = 1; i < weights.size(); ++i) {
        tmp = tmp.matmul(weights[i]);
        tmp += biases[i];

        // i + 1 due to the way we store activation functions.
        networkConfig.layersConfig[i + 1].activationFunction(tmp);
        activationResults.push_back(tmp);

        if (i == weights.size() - 1) {
            activationDerivResults.emplace_back();
        }
        else {
            auto tmpCopy = tmp;
            networkConfig.layersConfig[i + 1].activationDerivFunction(tmpCopy);
            activationDerivResults.push_back(tmpCopy);
        }
    }

    return StatsPrinter::getStats(tmp, labels);
}

void Network::backProp(const std::vector<unsigned int> &labels) {
    const auto &lastLayerConf = networkConfig.layersConfig[networkConfig.layersConfig.size() - 1];
    if (lastLayerConf.activationFunctionType != ActivationFunction::SoftMax) {
        throw WrongOutputActivationFunction();
    }

    size_t numLayers = networkConfig.layersConfig.size();

    deltaWeights[numLayers - 2] = CrossentropyFunction::costDelta(activationResults[numLayers - 1], labels);
    auto *lastDelta = &(deltaWeights[numLayers - 2]);

    for (int i = static_cast<int>(numLayers) - 2; i > 0; --i) {
        auto matmuls = lastDelta->matmul(weightsTransposed[i]);
        matmuls *= activationDerivResults[i - 1];
        deltaWeights[i - 1] = matmuls;
        lastDelta = &(deltaWeights[i - 1]);
    }

    for (size_t i = 0; i < numLayers - 1; ++i) {
        std::fill(deltaBiases[i].begin(), deltaBiases[i].end(), 0);
        for (size_t j = 0; j < deltaWeights[i].getNumRows(); ++j) {
            for (size_t k = 0; k < deltaWeights[i].getNumCols(); ++k) {
                deltaBiases[i][k] += deltaWeights[i].getItem(j, k) / deltaWeights[i].getNumRows();
            }
        }
    }
}

void Network::updateWeights(size_t batchSize, float eta) {
    optimizer->update(deltaWeights, activationResults, deltaBiases, batchSize, eta);
}

void Network::weightDecay(float eta, float lambda, size_t batchSize, size_t epoch) {
    if (lambda == 0) {
        return;
    }

    float decayCoeff = 1.f - lambda;

    for (auto &singleWeights : weights) {
        singleWeights *= decayCoeff;
    }
}

void Network::fit(const TrainValSplit_t &trainValSplit, size_t numEpochs, size_t batchSize, float eta, float lambda,
        uint8_t verboseLevel, LRScheduler *sched, size_t earlyStopping, long int maxTimeMs) {
    if (eta < 0) {
        throw NegativeEtaException();
    }
    auto startTime = std::chrono::high_resolution_clock::now();

    auto &train_X = trainValSplit.trainData;
    auto &train_y = trainValSplit.trainLabels;
    auto &validation_X = trainValSplit.validationData;
    auto &validation_y = trainValSplit.validationLabels;

    // Copy train data
    auto shuffledTrain_X = train_X;
    auto shuffledTrain_y = train_y;

    auto trainBatches_X = Matrix<float>::generateBatches(train_X, batchSize);
    auto trainBatches_y = Matrix<unsigned int>::generateVectorBatches(train_y, batchSize);

    float accSum = 0;
    float ceSum = 0;
    size_t numBatches = trainBatches_X.size();
    size_t t = 0;
    float currentBestCE = 10000;
    size_t epochOfBestCE = 0;

    sched->setEta(eta);

//    std::vector<unsigned int> labels(batchSize, 0);

    for (size_t i = 0; i < numEpochs; ++i) {
        // Reshuffle data
        auto shuffledData = DataManager::randomShuffle(std::move(shuffledTrain_X), std::move(shuffledTrain_y));
        shuffledTrain_X = std::move(shuffledData.data);
        shuffledTrain_y = std::move(shuffledData.vectorLabels);

        // Create new batches after reshuffling the data
        trainBatches_X = Matrix<float>::generateBatches(shuffledTrain_X, batchSize);
        trainBatches_y = Matrix<unsigned int>::generateVectorBatches(shuffledTrain_y, batchSize);

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t j = 0; j < numBatches; ++j) {
            eta = sched->exponential(t);

            auto stats = forwardPass(trainBatches_X[j], trainBatches_y[j]);
            accSum += stats.accuracy;
            ceSum += stats.crossEntropy;

            backProp(trainBatches_y[j]);
            weightDecay(eta, lambda, batchSize, i + 1);
            updateWeights(batchSize, eta);

            t += batchSize;
        }

//        auto valLabels = validation_y;
        auto predicted = predict(validation_X);
        auto valStats = StatsPrinter::getStats(predicted, validation_y);

        if (verboseLevel >= 3) {
            for (const auto &singleWeights : weights) {
                WeightInfo::printWeightStats(singleWeights, true);
            }
        }

        if (verboseLevel >= 1) {
            StatsPrinter::printProgressLine(accSum / static_cast<float>(numBatches),
                                            ceSum / static_cast<float>(numBatches),
                                            valStats.accuracy,
                                            valStats.crossEntropy,
                                            i + 1,
                                            numEpochs);

            accSum = 0;
            ceSum = 0;
        }

        if (verboseLevel >= 2) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

            std::cout << "Time taken by function: "
                      << duration.count() << " microseconds" << std::endl;
            std::cout << "ETA: " << eta << std::endl;
        }

        if (earlyStopping != 0){
            if (valStats.crossEntropy < currentBestCE){
                currentBestCE = valStats.crossEntropy;
                epochOfBestCE = i;
            }
            if (i - epochOfBestCE == earlyStopping){
                break;
            }
        }
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto currentDuration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();

        if (maxTimeMs != 0 && currentDuration >= maxTimeMs){
            if (verboseLevel >= 1){
                std::cout << "Time exceeded" << std::endl;
            }
            break;
        }
    }
}

Matrix<Network::ELEMENT_TYPE> Network::predict(const Matrix<float> &data) {
    auto tmp = data.matmul(weights[0]);
    tmp += biases[0];

    networkConfig.layersConfig[1].activationFunction(tmp);

    for (size_t i = 1; i < weights.size(); ++i) {
        tmp = tmp.matmul(weights[i]);
        tmp += biases[i];

        // i + 1 due to the way we store activation functions.
        networkConfig.layersConfig[i + 1].activationFunction(tmp);
    }

    return tmp;
}




// Parallel

// Do Forward and backwardpass alltogether
auto Network::forwardPassParallel(const std::vector<Matrix<ELEMENT_TYPE>> &data, const std::vector<std::vector<unsigned int>> &labels) {
    float acc = 0;
    float ce = 0;
    size_t batchSize = 0;
    for (auto &subBatch : data) batchSize += subBatch.getNumRows();

    auto printAcc = [](ELEMENT_TYPE acc) {std::cout << acc << std::endl;};
    auto copyPart = [](Matrix<ELEMENT_TYPE> &res, const Matrix<ELEMENT_TYPE> &part, size_t startRow) {
//        std::cout << res.getNumCols() << " - " << part.getNumCols() << std::endl;
        if (res.getNumCols() != part.getNumCols())
            throw std::exception();

        for (size_t i = startRow; i < startRow + part.getNumRows(); ++i) {
            for (size_t j = 0; j < part.getNumCols(); ++j) {
                res.setItem(i, j, part.getItem(i - startRow, j));
            }
        }
    };
    auto printNumCols = [](const Matrix<ELEMENT_TYPE> &mat) {
        std::cout << mat.getNumCols() << std::endl;
    };

    size_t currentStartRows = 0;
    std::vector<size_t> startRows;
    for (auto &d : data) {
        startRows.push_back(currentStartRows);
        currentStartRows += d.getNumRows();
    }

    activationResults.clear();
    activationDerivResults.clear();
    deltaWeights.clear();
    deltaBiases.clear();

    activationResults.emplace_back(batchSize, weights[0].getNumRows(), 0);
    for (auto & weight : weights) {
        activationResults.emplace_back(batchSize, weight.getNumCols(), 0);
        activationDerivResults.emplace_back(batchSize, weight.getNumCols(), 0);
        deltaWeights.emplace_back(batchSize, weight.getNumCols(), 0);
        deltaBiases.emplace_back(weight.getNumCols());

        if (weight.getNumCols() == 0) {
            throw std::exception();
        }
    }

    for (auto &deltaBias : deltaBiases) {
        std::fill(deltaBias.begin(), deltaBias.end(), 0);
    }

#pragma omp parallel for default(none) shared(acc, ce, data, labels, printAcc, copyPart, startRows, parallelActivationResults, parallelActivationDerivResults, deltaWeights, deltaBiases, activationResults, activationDerivResults, networkConfig, weightsTransposed, printNumCols)
//    {
        for (size_t k = 0; k < numThreads; ++k) {
            // Last batch might be smaller.
            if (data.size() - 1 < k)
                continue;

            parallelActivationResults[k].clear();
            parallelActivationDerivResults[k].clear();
            // Input layer has activation fn equal to identity.
            parallelActivationResults[k].push_back(data[k]);
//            parallelActivationResults[k][0] = data[k];

            copyPart(activationResults[0], data[k], startRows[k]);

            auto tmp = data[k].matmul(weights[0]);
            tmp += biases[0];

            networkConfig.layersConfig[1].activationFunction(tmp);
            parallelActivationResults[k].push_back(tmp);
//            parallelActivationResults[k][1] = tmp;

            copyPart(activationResults[1], tmp, startRows[k]);

            if (weights.size() == 1) {
                parallelActivationDerivResults[k].emplace_back();
            }
            else {
                auto tmpCopy = tmp;
                networkConfig.layersConfig[1].activationDerivFunction(tmpCopy);
                parallelActivationDerivResults[k].push_back(tmpCopy);
                copyPart(activationDerivResults[0], tmpCopy, startRows[k]);
            }

            for (size_t i = 1; i < weights.size(); ++i) {
                tmp = tmp.matmul(weights[i]);
                tmp += biases[i];

                // k + 1 due to the way we store activation functions.
                networkConfig.layersConfig[i + 1].activationFunction(tmp);
                parallelActivationResults[k].push_back(tmp);
//                parallelActivationResults[k][i + 1] = tmp;

                copyPart(activationResults[i + 1], tmp, startRows[k]);

                if (i == weights.size() - 1) {
                    parallelActivationDerivResults[k].emplace_back();
                }
                else {
                    auto tmpCopy = tmp;
                    networkConfig.layersConfig[i + 1].activationDerivFunction(tmpCopy);
                    parallelActivationDerivResults[k].push_back(tmpCopy);
                    copyPart(activationDerivResults[i], tmpCopy, startRows[k]);
                }
            }

            auto stats = StatsPrinter::getStats(tmp, labels[k]);
//            printAcc(stats.accuracy);

#pragma omp critical
            {
                acc += stats.accuracy;
                ce += stats.crossEntropy;
            };

//            // Do backprop and then aggregate values into deltaWeights and deltaBiases
            const auto &lastLayerConf = networkConfig.layersConfig[networkConfig.layersConfig.size() - 1];
            if (lastLayerConf.activationFunctionType != ActivationFunction::SoftMax) {
                throw WrongOutputActivationFunction();
            }

            size_t numLayers = networkConfig.layersConfig.size();

            parallelDeltaWeights[k][numLayers - 2] = CrossentropyFunction::costDelta(parallelActivationResults[k][numLayers - 1], labels[k]);
            auto *lastDelta = &(parallelDeltaWeights[k][numLayers - 2]);

            copyPart(deltaWeights[numLayers - 2], parallelDeltaWeights[k][numLayers - 2], startRows[k]);

            for (int i = static_cast<int>(numLayers) - 2; i > 0; --i) {
                auto matmuls = lastDelta->matmul(weightsTransposed[i]);
                matmuls *= parallelActivationDerivResults[k][i - 1];
                parallelDeltaWeights[k][i - 1] = matmuls;
                lastDelta = &(parallelDeltaWeights[k][i - 1]);

                copyPart(deltaWeights[i - 1], parallelDeltaWeights[k][i - 1], startRows[k]);
            }

            for (size_t i = 0; i < numLayers - 1; ++i) {
                std::fill(parallelDeltaBiases[k][i].begin(), parallelDeltaBiases[k][i].end(), 0);
                for (size_t j = 0; j < parallelDeltaWeights[k][i].getNumRows(); ++j) {
#pragma omp simd
                    for (size_t l = 0; l < parallelDeltaWeights[k][i].getNumCols(); ++l) {
                        parallelDeltaBiases[k][i][l] += parallelDeltaWeights[k][i].getItem(j, l);
                    }
                }

#pragma omp critical
                {
#pragma omp simd
                    for (size_t j = 0; j < deltaBiases[i].size(); ++j) {
                        deltaBiases[i][j] += parallelDeltaBiases[k][i][j] / static_cast<float>(data[k].getNumRows());
                    }
                };
            }
        }
//    }

#pragma omp barrier

//    return StatsPrinter::getStats(tmp, labels);
    return Stats{.accuracy=acc / numThreads, .crossEntropy=ce / numThreads};

}

auto Network::predictParallel(const std::vector<Matrix<float>> &dataBatches, const std::vector<std::vector<unsigned int>> &labels) {
    float acc = 0;
    float ce = 0;

#pragma omp parallel for default(none) shared(dataBatches, labels, acc, ce)
    for (size_t k = 0; k < numThreads; ++k) {
        auto &data = dataBatches[k];

        auto tmp = data.matmul(weights[0]);
        tmp += biases[0];

        networkConfig.layersConfig[1].activationFunction(tmp);

        for (size_t i = 1; i < weights.size(); ++i) {
            tmp = tmp.matmul(weights[i]);
            tmp += biases[i];

            // i + 1 due to the way we store activation functions.
            networkConfig.layersConfig[i + 1].activationFunction(tmp);
        }

        auto stats = StatsPrinter::getStats(tmp, labels[k]);

#pragma omp critical
        {
            acc += stats.accuracy;
            ce += stats.crossEntropy;
        };
    }

#pragma omp barrier

    return Stats{.accuracy=acc / numThreads, .crossEntropy=ce / numThreads};
}

void Network::weightDecayParallel(float lambda) {
    if (lambda == 0)
        return;

    float decayCoeff = 1.f - lambda;

#pragma omp parallel for default(none) shared(decayCoeff, weights)
    for (size_t i = 0; i < weights.size(); ++i) {
        weights[i] *= decayCoeff;
    }
#pragma omp barrier
}

void Network::parallelFit(const TrainValSplit_t &trainValSplit, size_t numEpochs, size_t batchSize, float eta, float lambda,
                          uint8_t verboseLevel, LRScheduler *sched, size_t earlyStopping, long int maxTimeMs) {
    if (eta < 0) {
        throw NegativeEtaException();
    }
    auto startTime = std::chrono::high_resolution_clock::now();

    auto &train_X = trainValSplit.trainData;
    auto &train_y = trainValSplit.trainLabels;
    auto &validation_X = trainValSplit.validationData;
    auto &validation_y = trainValSplit.validationLabels;

    auto validationBatches_X = Matrix<float>::generateBatches(validation_X, validation_X.getNumRows() / numThreads);
    auto validationBatches_y = Matrix<unsigned int>::generateVectorBatches(validation_y, validation_y.size() / numThreads);

    // Copy train data
    auto shuffledTrain_X = train_X;
    auto shuffledTrain_y = train_y;

    auto trainBatches_X = Matrix<float>::generateBatches(train_X, batchSize);
    auto trainBatches_y = Matrix<unsigned int>::generateVectorBatches(train_y, batchSize);

    float accSum = 0;
    float ceSum = 0;
    size_t numBatches = trainBatches_X.size();
    size_t t = 0;
    float currentBestCE = 10000;
    size_t epochOfBestCE = 0;

    sched->setEta(eta);

//    std::vector<unsigned int> labels(batchSize, 0);

    for (size_t i = 0; i < numEpochs; ++i) {
        // Reshuffle data
        auto shuffledData = DataManager::randomShuffle(std::move(shuffledTrain_X), std::move(shuffledTrain_y));
        shuffledTrain_X = std::move(shuffledData.data);
        shuffledTrain_y = std::move(shuffledData.vectorLabels);

        // Create new batches after reshuffling the data
        trainBatches_X = Matrix<float>::generateBatches(shuffledTrain_X, batchSize);
        trainBatches_y = Matrix<unsigned int>::generateVectorBatches(shuffledTrain_y, batchSize);

        std::vector<std::vector<Matrix<float>>> parallelBatches_X;
        std::vector<std::vector<std::vector<unsigned int>>> parallelBatches_y;

        for (auto &batch : trainBatches_X) {
            parallelBatches_X.emplace_back(Matrix<float>::generateBatches(batch, batchSize / numThreads));
        }

        for (auto &batch : trainBatches_y) {
            parallelBatches_y.emplace_back(Matrix<unsigned int>::generateVectorBatches(batch, batchSize / numThreads));
        }

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t j = 0; j < numBatches; ++j) {
            eta = sched->exponential(t);

            // ToDo: Optimise
//            auto labels = trainBatches_y[j].getMatrixCol(0);
            auto stats = forwardPassParallel(parallelBatches_X[j], parallelBatches_y[j]);
            accSum += stats.accuracy;
            ceSum += stats.crossEntropy;

//            backProp(trainBatches_y[j]);
            weightDecayParallel(lambda);
            updateWeights(batchSize, eta);

            t += batchSize;
        }

//        auto valLabels = validation_y;
        auto valStats = predictParallel(validationBatches_X, validationBatches_y);
//        auto predicted = predict(validation_X);
//        auto valStats = StatsPrinter::getStats(predicted, validation_y);

        if (verboseLevel >= 3) {
            for (const auto &singleWeights : weights) {
                WeightInfo::printWeightStats(singleWeights, true);
            }
        }

        if (verboseLevel >= 1) {
            StatsPrinter::printProgressLine(accSum / static_cast<float>(numBatches),
                                            ceSum / static_cast<float>(numBatches),
                                            valStats.accuracy,
                                            valStats.crossEntropy,
                                            i + 1,
                                            numEpochs);

            accSum = 0;
            ceSum = 0;
        }

        if (verboseLevel >= 2) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

            std::cout << "Time taken by function: "
                      << duration.count() << " microseconds" << std::endl;
            std::cout << "ETA: " << eta << std::endl;
        }

        if (earlyStopping != 0){
            if (valStats.crossEntropy < currentBestCE){
                currentBestCE = valStats.crossEntropy;
                epochOfBestCE = i;
            }
            if (i - epochOfBestCE == earlyStopping){
                break;
            }
        }
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto currentDuration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();

        if (maxTimeMs != 0 && currentDuration >= maxTimeMs){
            if (verboseLevel >= 1){
                std::cout << "Time exceeded" << std::endl;
            }
            break;
        }
    }
}